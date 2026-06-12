#define _POSIX_C_SOURCE 200809L

#include "ipc_socket.h"

#include "binary_io.h"
#include "duration_format.h"
#include "sysfs_poll.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define IPC_HEADER_ASCII      0x51U
#define IPC_HEADER_ASCII_ALL  0x52U
#define IPC_HEADER_JSON       0x4AU
#define IPC_HEADER_JSON_ALL   0x4BU
#define IPC_JSON_BUF     640
#define IPC_ASCII_BUF    384

#ifdef TEST_MODE
#define IPC_SOCKET_PATH "./tmp/power_ledger.sock"
#else
#define IPC_SOCKET_PATH "/run/power_ledger.sock"
#endif

struct IpcSocket {
    int listen_fd;
    struct PowerLedgerEvent cache;
    int cache_valid;
    uint8_t ac_online;
    int ac_online_valid;
    char ledger_path[256];
    int ledger_path_valid;
    uint32_t session_active_seconds;
    uint32_t session_all_active_seconds;
    uint8_t session_ac_online_snapshot;
    int session_ac_online_snapshot_valid;
    struct PowerLedgerEvent session_anchor;
    int session_anchor_valid;
    int32_t remaining_mins;
    int32_t to_full_mins;
    pthread_mutex_t lock;
    pthread_t listener_thread;
    volatile int listener_stop;
    int listener_started;
};

const char *ipc_socket_path(void)
{
    return IPC_SOCKET_PATH;
}

static const char *regime_to_string(uint8_t regime)
{
    switch ((power_regime_t)regime) {
    case REG_PERFORMANCE:
        return "performance";
    case REG_BALANCED:
        return "balanced";
    case REG_POWER_SAVE:
        return "power-save";
    case REG_QUIET:
        return "quiet";
    default:
        return "balanced";
    }
}

static const char *status_from_power(int32_t power_uw)
{
    if (power_uw < 0) {
        return "discharging";
    }
    if (power_uw > 0) {
        return "charging";
    }
    return "idle";
}

static int ipc_socket_service_client(IpcSocket *ipc, int client_fd);

static int ensure_socket_parent_dir(void)
{
#ifdef TEST_MODE
    if (mkdir("tmp", 0700) != 0 && errno != EEXIST) {
        return -1;
    }
#endif
    return 0;
}

static int bind_listen_socket(int *out_fd)
{
    struct sockaddr_un addr;
    int fd;
    int one = 1;

    if (out_fd == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (ensure_socket_parent_dir() != 0) {
        return -1;
    }

    (void)unlink(IPC_SOCKET_PATH);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
        (void)close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", IPC_SOCKET_PATH) >=
        (int)sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        (void)close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        (void)close(fd);
        return -1;
    }

    if (chmod(IPC_SOCKET_PATH, 0666) != 0) {
        (void)close(fd);
        (void)unlink(IPC_SOCKET_PATH);
        return -1;
    }

    if (listen(fd, 16) != 0) {
        (void)close(fd);
        (void)unlink(IPC_SOCKET_PATH);
        return -1;
    }

    *out_fd = fd;
    return 0;
}

static uint32_t monotonic_now_seconds(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
        return 0U;
    }

    return (uint32_t)ts.tv_sec;
}

static const char *ipc_ledger_path(const IpcSocket *ipc)
{
    if (ipc != NULL && ipc->ledger_path_valid && ipc->ledger_path[0] != '\0') {
        return ipc->ledger_path;
    }

    return BINARY_IO_DEFAULT_PATH;
}

static void ipc_apply_cache_defaults(IpcSocket *ipc)
{
    if (ipc == NULL) {
        return;
    }

    memset(&ipc->cache, 0, sizeof(ipc->cache));
    ipc->cache.type = (uint8_t)EV_TICK;
    ipc->cache.power_regime = (uint8_t)REG_BALANCED;
    ipc->cache.timestamp = monotonic_now_seconds();
    ipc->cache_valid = 1;
    ipc->remaining_mins = SYSFS_ETA_NA;
    ipc->to_full_mins = SYSFS_ETA_NA;
}

static void ipc_write_fallback_json(char *buf, size_t buflen)
{
    if (buf == NULL || buflen == 0U) {
        return;
    }

    (void)snprintf(buf, buflen,
                   "{\"status\":\"idle\",\"pct\":0,\"watts\":0.00,\"regime\":\"balanced\","
                   "\"rpm\":0,\"session_mins\":0}\n");
}

static void ipc_write_fallback_ascii(char *buf, size_t buflen)
{
    if (buf == NULL || buflen == 0U) {
        return;
    }

    (void)snprintf(buf, buflen,
                   "status=idle pct=0 watts=0.00 regime=balanced rpm=0 session_mins=0 ts=0\n");
}

static void ipc_store_ledger_path(IpcSocket *ipc, const char *ledger_path)
{
    const char *target;

    if (ipc == NULL) {
        return;
    }

    target = (ledger_path != NULL) ? ledger_path : BINARY_IO_DEFAULT_PATH;
    if (snprintf(ipc->ledger_path, sizeof(ipc->ledger_path), "%s", target) >=
        (int)sizeof(ipc->ledger_path)) {
        ipc->ledger_path_valid = 0;
        return;
    }

    ipc->ledger_path_valid = 1;
}

static int ipc_on_ac_now(const IpcSocket *ipc)
{
    if (ipc == NULL) {
        return 0;
    }

    if (ipc->ac_online_valid && ipc->ac_online != 0U) {
        return 1;
    }

    if (ipc->cache_valid && ipc->cache.power_drain >= 0) {
        return 1;
    }

    return 0;
}

static void session_refresh_caches_from_ledger(IpcSocket *ipc)
{
    const char *path;
    struct BinaryIoDischargeResult discharge;
    uint32_t all_seconds = 0U;
    int on_ac_now;

    if (ipc == NULL) {
        return;
    }

    path = ipc_ledger_path(ipc);
    on_ac_now = ipc_on_ac_now(ipc);

    if (binary_io_analyze_discharge(path, on_ac_now, &discharge) != 0) {
        return;
    }

    if (binary_io_compute_session_active(path, 1, &all_seconds, NULL, NULL) != 0) {
        return;
    }

    ipc->session_active_seconds = discharge.active_seconds;
    ipc->session_all_active_seconds = all_seconds;
    ipc->session_anchor_valid = 0;

    if (ipc->ac_online_valid) {
        ipc->session_ac_online_snapshot = ipc->ac_online;
        ipc->session_ac_online_snapshot_valid = 1;
    }
}

#define SESSION_TICK_DELTA_MAX 180U

static void session_advance_tick(IpcSocket *ipc, const struct PowerLedgerEvent *event)
{
    const struct PowerLedgerEvent *prev;
    uint32_t delta;

    if (ipc == NULL || event == NULL) {
        return;
    }

    if (!ipc->cache_valid) {
        session_refresh_caches_from_ledger(ipc);
        return;
    }

    prev = &ipc->cache;
    if (event->timestamp <= prev->timestamp) {
        return;
    }

    delta = event->timestamp - prev->timestamp;
    if (delta > SESSION_TICK_DELTA_MAX) {
        session_refresh_caches_from_ledger(ipc);
        return;
    }

    if (ipc_on_ac_now(ipc)) {
        ipc->session_active_seconds = 0U;
    } else {
        ipc->session_active_seconds += delta;
    }

    ipc->session_all_active_seconds += delta;
}

static void session_apply_ac_state(IpcSocket *ipc)
{
    if (ipc == NULL || !ipc->ac_online_valid) {
        return;
    }

    if (ipc->ac_online != 0U) {
        ipc->session_active_seconds = 0U;
    }

    ipc->session_ac_online_snapshot = ipc->ac_online;
    ipc->session_ac_online_snapshot_valid = 1;
}

static void ipc_socket_update_cache_unlocked(IpcSocket *ipc,
                                             const struct PowerLedgerEvent *event,
                                             const struct SysfsSample *sample,
                                             int advance_session)
{
    if (ipc == NULL || event == NULL) {
        return;
    }

    if (sample != NULL) {
        ipc->remaining_mins = sample->remaining_mins;
        ipc->to_full_mins = sample->to_full_mins;
        ipc->ac_online = sample->ac_online;
        ipc->ac_online_valid = 1;
    } else {
        ipc->remaining_mins = SYSFS_ETA_NA;
        ipc->to_full_mins = SYSFS_ETA_NA;
        ipc->ac_online_valid = 0;
    }

    if (advance_session) {
        if (event->type == (uint8_t)EV_TICK) {
            session_advance_tick(ipc, event);
        } else {
            session_refresh_caches_from_ledger(ipc);
        }
    }

    memcpy(&ipc->cache, event, sizeof(ipc->cache));
    ipc->cache_valid = 1;
}

static int ipc_socket_refresh_live_unlocked(IpcSocket *ipc)
{
    struct SysfsSample sample;
    struct PowerLedgerEvent event;

    if (ipc == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (sysfs_poll_sample(&sample) != 0) {
        if (!ipc->cache_valid) {
            ipc_apply_cache_defaults(ipc);
        }
        return -1;
    }

    sysfs_poll_build_event(EV_TICK, &sample, &event);
    ipc_socket_update_cache_unlocked(ipc, &event, &sample, 0);
    return 0;
}

int ipc_socket_refresh_live(IpcSocket *ipc)
{
    int rc;

    if (ipc == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&ipc->lock);
    rc = ipc_socket_refresh_live_unlocked(ipc);
    pthread_mutex_unlock(&ipc->lock);
    return rc;
}

static int format_eta_json_suffix(int32_t remaining_mins, int32_t to_full_mins,
                                  char *buf, size_t buflen)
{
    char duration[32];
    size_t used = 0U;

    if (buf == NULL || buflen == 0U) {
        errno = EINVAL;
        return -1;
    }

    buf[0] = '\0';

    if (remaining_mins >= 0) {
        duration_format_mins(remaining_mins, duration, sizeof(duration));
        if (snprintf(buf + used, buflen - used, ",\"remaining\":\"%s\"", duration) >=
            (int)(buflen - used)) {
            errno = ENOMEM;
            return -1;
        }
        used = strlen(buf);
    }

    if (to_full_mins >= 0) {
        duration_format_mins(to_full_mins, duration, sizeof(duration));
        if (snprintf(buf + used, buflen - used, ",\"to_full\":\"%s\"", duration) >=
            (int)(buflen - used)) {
            errno = ENOMEM;
            return -1;
        }
    }

    return 0;
}

static int format_eta_ascii_suffix(int32_t remaining_mins, int32_t to_full_mins,
                                   char *buf, size_t buflen)
{
    char duration[32];
    size_t used = 0U;

    if (buf == NULL || buflen == 0U) {
        errno = EINVAL;
        return -1;
    }

    buf[0] = '\0';

    if (remaining_mins >= 0) {
        duration_format_mins(remaining_mins, duration, sizeof(duration));
        if (snprintf(buf + used, buflen - used, " remaining=%s", duration) >=
            (int)(buflen - used)) {
            errno = ENOMEM;
            return -1;
        }
        used = strlen(buf);
    }

    if (to_full_mins >= 0) {
        duration_format_mins(to_full_mins, duration, sizeof(duration));
        if (snprintf(buf + used, buflen - used, " to_full=%s", duration) >=
            (int)(buflen - used)) {
            errno = ENOMEM;
            return -1;
        }
    }

    return 0;
}

static int build_json_payload(IpcSocket *ipc, char *buf, size_t buflen, int scan_all)
{
    const struct PowerLedgerEvent *event;
    char eta_suffix[96];
    double watts;
    unsigned session_mins;

    if (ipc == NULL || buf == NULL || buflen == 0U) {
        errno = EINVAL;
        return -1;
    }

    (void)ipc_socket_refresh_live_unlocked(ipc);
    if (!ipc->cache_valid) {
        ipc_apply_cache_defaults(ipc);
    }

    session_apply_ac_state(ipc);

    event = &ipc->cache;

    if (format_eta_json_suffix(ipc->remaining_mins, ipc->to_full_mins,
                               eta_suffix, sizeof(eta_suffix)) != 0) {
        return -1;
    }

    watts = (double)event->power_drain / 1000000.0;
    session_mins = (scan_all ? ipc->session_all_active_seconds
                             : ipc->session_active_seconds) /
                   60U;

    if (snprintf(buf, buflen,
                 "{\"status\":\"%s\",\"pct\":%u,\"watts\":%.2f,\"regime\":\"%s\","
                 "\"rpm\":%u,\"session_mins\":%u%s}\n",
                 status_from_power(event->power_drain),
                 (unsigned)event->battery_level,
                 watts,
                 regime_to_string(event->power_regime),
                 (unsigned)event->fan_speed,
                 session_mins,
                 eta_suffix) >= (int)buflen) {
        errno = ENOMEM;
        return -1;
    }

    return 0;
}

static int build_ascii_payload(IpcSocket *ipc, char *buf, size_t buflen, int scan_all)
{
    const struct PowerLedgerEvent *event;
    char eta_suffix[96];
    double watts;
    unsigned session_mins;

    if (ipc == NULL || buf == NULL || buflen == 0U) {
        errno = EINVAL;
        return -1;
    }

    (void)ipc_socket_refresh_live_unlocked(ipc);
    if (!ipc->cache_valid) {
        ipc_apply_cache_defaults(ipc);
    }

    session_apply_ac_state(ipc);

    event = &ipc->cache;

    if (format_eta_ascii_suffix(ipc->remaining_mins, ipc->to_full_mins,
                                eta_suffix, sizeof(eta_suffix)) != 0) {
        return -1;
    }

    watts = (double)event->power_drain / 1000000.0;
    session_mins = (scan_all ? ipc->session_all_active_seconds
                             : ipc->session_active_seconds) /
                   60U;

    if (snprintf(buf, buflen,
                 "status=%s pct=%u watts=%.2f regime=%s rpm=%u session_mins=%u ts=%u%s\n",
                 status_from_power(event->power_drain),
                 (unsigned)event->battery_level,
                 watts,
                 regime_to_string(event->power_regime),
                 (unsigned)event->fan_speed,
                 session_mins,
                 (unsigned)event->timestamp,
                 eta_suffix) >= (int)buflen) {
        errno = ENOMEM;
        return -1;
    }

    return 0;
}

static int set_blocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}

static int read_request_byte_blocking(int client_fd, unsigned char *header)
{
    struct pollfd pfd;
    ssize_t nread;
    int tries;

    if (header == NULL) {
        errno = EINVAL;
        return -1;
    }

    pfd.fd = client_fd;
    pfd.events = POLLIN;

    for (tries = 0; tries < 100; tries++) {
        nread = read(client_fd, header, 1);
        if (nread == 1) {
            return 0;
        }
        if (nread == 0) {
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        if (poll(&pfd, 1, 50) <= 0) {
            return -1;
        }
    }

    errno = ETIMEDOUT;
    return -1;
}

static void ipc_send_response(int client_fd, const char *payload)
{
    struct pollfd pfd;
    size_t out_len;
    ssize_t nwritten;
    size_t sent = 0U;
    int tries;

    if (payload == NULL) {
        return;
    }

    out_len = strlen(payload);
    if (out_len == 0U) {
        return;
    }

    pfd.fd = client_fd;
    pfd.events = POLLOUT;

    for (tries = 0; sent < out_len && tries < 200; tries++) {
        nwritten = write(client_fd, payload + sent, out_len - sent);
        if (nwritten > 0) {
            sent += (size_t)nwritten;
            continue;
        }
        if (nwritten == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        }
        if (poll(&pfd, 1, 50) <= 0) {
            break;
        }
    }
}

static void *ipc_listener_thread(void *arg)
{
    IpcSocket *ipc = arg;

    if (ipc == NULL) {
        return NULL;
    }

    while (ipc->listener_stop == 0) {
        int client_fd = accept(ipc->listen_fd, NULL, NULL);

        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (ipc->listener_stop) {
                break;
            }
            perror("ipc accept");
            continue;
        }

        pthread_mutex_lock(&ipc->lock);
        (void)ipc_socket_service_client(ipc, client_fd);
        pthread_mutex_unlock(&ipc->lock);
        (void)close(client_fd);
    }

    return NULL;
}

static int ipc_socket_service_client(IpcSocket *ipc, int client_fd)
{
    unsigned char header;
    char payload[IPC_JSON_BUF];

    if (ipc == NULL || client_fd < 0) {
        errno = EINVAL;
        return -1;
    }

    (void)set_blocking(client_fd);

    memset(payload, 0, sizeof(payload));

    if (read_request_byte_blocking(client_fd, &header) != 0) {
        return 0;
    }

    if (header == IPC_HEADER_JSON) {
        if (build_json_payload(ipc, payload, sizeof(payload), 0) != 0) {
            ipc_write_fallback_json(payload, sizeof(payload));
        }
        ipc_send_response(client_fd, payload);
    } else if (header == IPC_HEADER_JSON_ALL) {
        if (build_json_payload(ipc, payload, sizeof(payload), 1) != 0) {
            ipc_write_fallback_json(payload, sizeof(payload));
        }
        ipc_send_response(client_fd, payload);
    } else if (header == IPC_HEADER_ASCII) {
        if (build_ascii_payload(ipc, payload, sizeof(payload), 0) != 0) {
            ipc_write_fallback_ascii(payload, sizeof(payload));
        }
        ipc_send_response(client_fd, payload);
    } else if (header == IPC_HEADER_ASCII_ALL) {
        if (build_ascii_payload(ipc, payload, sizeof(payload), 1) != 0) {
            ipc_write_fallback_ascii(payload, sizeof(payload));
        }
        ipc_send_response(client_fd, payload);
    }

    return 0;
}

int ipc_socket_init(IpcSocket **out)
{
    IpcSocket *ipc;

    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }

    ipc = calloc(1, sizeof(*ipc));
    if (ipc == NULL) {
        return -1;
    }

    ipc->listen_fd = -1;
    ipc->cache_valid = 0;
    ipc_store_ledger_path(ipc, NULL);
    ipc->session_active_seconds = 0U;
    ipc->session_all_active_seconds = 0U;
    ipc->session_ac_online_snapshot_valid = 0;
    ipc->session_anchor_valid = 0;
    ipc->remaining_mins = SYSFS_ETA_NA;
    ipc->to_full_mins = SYSFS_ETA_NA;
    ipc->listener_stop = 0;
    ipc->listener_started = 0;

    if (pthread_mutex_init(&ipc->lock, NULL) != 0) {
        free(ipc);
        return -1;
    }

    if (bind_listen_socket(&ipc->listen_fd) != 0) {
        pthread_mutex_destroy(&ipc->lock);
        free(ipc);
        return -1;
    }

    if (pthread_create(&ipc->listener_thread, NULL, ipc_listener_thread, ipc) != 0) {
        (void)close(ipc->listen_fd);
        ipc->listen_fd = -1;
        (void)unlink(IPC_SOCKET_PATH);
        pthread_mutex_destroy(&ipc->lock);
        free(ipc);
        return -1;
    }

    ipc->listener_started = 1;
    *out = ipc;
    return 0;
}

void ipc_socket_restore_session(IpcSocket *ipc, const char *ledger_path)
{
    if (ipc == NULL) {
        return;
    }

    pthread_mutex_lock(&ipc->lock);
    ipc_store_ledger_path(ipc, ledger_path);
    session_refresh_caches_from_ledger(ipc);
    pthread_mutex_unlock(&ipc->lock);
}

void ipc_socket_update_cache(IpcSocket *ipc, const struct PowerLedgerEvent *event,
                             const struct SysfsSample *sample, int advance_session)
{
    if (ipc == NULL || event == NULL) {
        return;
    }

    pthread_mutex_lock(&ipc->lock);
    ipc_socket_update_cache_unlocked(ipc, event, sample, advance_session);
    pthread_mutex_unlock(&ipc->lock);
}

int ipc_socket_accept_pending(IpcSocket *ipc, int epfd)
{
    int client_fd;

    (void)epfd;

    if (ipc == NULL || ipc->listen_fd < 0) {
        errno = EINVAL;
        return -1;
    }

    client_fd = accept(ipc->listen_fd, NULL, NULL);
    if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return -1;
    }

    pthread_mutex_lock(&ipc->lock);
    (void)ipc_socket_service_client(ipc, client_fd);
    pthread_mutex_unlock(&ipc->lock);
    (void)close(client_fd);
    return 0;
}

void ipc_socket_shutdown(IpcSocket *ipc, int epfd)
{
    (void)epfd;

    if (ipc == NULL) {
        return;
    }

    if (ipc->listener_started) {
        ipc->listener_stop = 1;
        if (ipc->listen_fd >= 0) {
            (void)shutdown(ipc->listen_fd, SHUT_RDWR);
        }
        (void)pthread_join(ipc->listener_thread, NULL);
        ipc->listener_started = 0;
    }

    if (ipc->listen_fd >= 0) {
        (void)close(ipc->listen_fd);
        ipc->listen_fd = -1;
    }

    (void)unlink(IPC_SOCKET_PATH);
    pthread_mutex_destroy(&ipc->lock);
    free(ipc);
}

int ipc_socket_get_listen_fd(const IpcSocket *ipc)
{
    if (ipc == NULL) {
        errno = EINVAL;
        return -1;
    }

    return ipc->listen_fd;
}

#ifdef TEST_MODE
int ipc_socket_test_build_json(IpcSocket *ipc, char *buf, size_t buflen)
{
    int rc;

    if (ipc == NULL || buf == NULL || buflen == 0U) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&ipc->lock);
    rc = build_json_payload(ipc, buf, buflen, 0);
    pthread_mutex_unlock(&ipc->lock);
    return rc;
}

int ipc_socket_test_build_json_all(IpcSocket *ipc, char *buf, size_t buflen)
{
    int rc;

    if (ipc == NULL || buf == NULL || buflen == 0U) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&ipc->lock);
    rc = build_json_payload(ipc, buf, buflen, 1);
    pthread_mutex_unlock(&ipc->lock);
    return rc;
}

int ipc_socket_test_build_ascii(IpcSocket *ipc, char *buf, size_t buflen)
{
    int rc;

    if (ipc == NULL || buf == NULL || buflen == 0U) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&ipc->lock);
    rc = build_ascii_payload(ipc, buf, buflen, 0);
    pthread_mutex_unlock(&ipc->lock);
    return rc;
}

int ipc_socket_test_build_ascii_all(IpcSocket *ipc, char *buf, size_t buflen)
{
    int rc;

    if (ipc == NULL || buf == NULL || buflen == 0U) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&ipc->lock);
    rc = build_ascii_payload(ipc, buf, buflen, 1);
    pthread_mutex_unlock(&ipc->lock);
    return rc;
}
#endif
