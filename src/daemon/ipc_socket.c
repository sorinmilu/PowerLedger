#define _POSIX_C_SOURCE 200809L

#include "ipc_socket.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define IPC_HEADER_ASCII 0x51U
#define IPC_HEADER_JSON  0x4AU
#define IPC_JSON_BUF     512
#define IPC_ASCII_BUF    256

#ifdef TEST_MODE
#define IPC_SOCKET_PATH "./tmp/power_ledger.sock"
#else
#define IPC_SOCKET_PATH "/run/power_ledger.sock"
#endif

struct IpcSocket {
    int listen_fd;
    struct PowerLedgerEvent cache;
    int cache_valid;
    uint32_t session_start_ts;
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

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

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

    if (set_nonblocking(fd) != 0) {
        (void)close(fd);
        (void)unlink(IPC_SOCKET_PATH);
        return -1;
    }

    *out_fd = fd;
    return 0;
}

static int build_json_payload(const struct PowerLedgerEvent *event, uint32_t session_start,
                              char *buf, size_t buflen)
{
    double watts;
    unsigned session_mins;
    uint32_t now_ts;

    if (event == NULL || buf == NULL || buflen == 0U) {
        errno = EINVAL;
        return -1;
    }

    watts = (double)event->power_drain / 1000000.0;
    now_ts = event->timestamp;
    if (session_start > 0U && now_ts >= session_start) {
        session_mins = (now_ts - session_start) / 60U;
    } else {
        session_mins = 0U;
    }

    if (snprintf(buf, buflen,
                 "{\"status\":\"%s\",\"pct\":%u,\"watts\":%.2f,\"regime\":\"%s\","
                 "\"rpm\":%u,\"session_mins\":%u}\n",
                 status_from_power(event->power_drain),
                 (unsigned)event->battery_level,
                 watts,
                 regime_to_string(event->power_regime),
                 (unsigned)event->fan_speed,
                 session_mins) >= (int)buflen) {
        errno = ENOMEM;
        return -1;
    }

    return 0;
}

static int build_ascii_payload(const struct PowerLedgerEvent *event, char *buf, size_t buflen)
{
    double watts;

    if (event == NULL || buf == NULL || buflen == 0U) {
        errno = EINVAL;
        return -1;
    }

    watts = (double)event->power_drain / 1000000.0;

    if (snprintf(buf, buflen,
                 "status=%s pct=%u watts=%.2f regime=%s rpm=%u ts=%u\n",
                 status_from_power(event->power_drain),
                 (unsigned)event->battery_level,
                 watts,
                 regime_to_string(event->power_regime),
                 (unsigned)event->fan_speed,
                 (unsigned)event->timestamp) >= (int)buflen) {
        errno = ENOMEM;
        return -1;
    }

    return 0;
}

static int read_request_byte(int client_fd, unsigned char *out_byte)
{
    ssize_t nread;
    unsigned char byte;

    if (out_byte == NULL) {
        errno = EINVAL;
        return -1;
    }

    for (;;) {
        nread = read(client_fd, &byte, 1);
        if (nread == 1) {
            *out_byte = byte;
            return 0;
        }
        if (nread == 0) {
            errno = ECONNRESET;
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1;
        }
        return -1;
    }
}

static void service_client(IpcSocket *ipc, int client_fd)
{
    unsigned char header;
    char payload[IPC_JSON_BUF];
    ssize_t nwritten;
    const char *out;
    size_t out_len;

    if (ipc == NULL || client_fd < 0) {
        return;
    }

    if (read_request_byte(client_fd, &header) != 0) {
        (void)close(client_fd);
        return;
    }

    if (!ipc->cache_valid) {
        (void)close(client_fd);
        return;
    }

    memset(payload, 0, sizeof(payload));

    if (header == IPC_HEADER_JSON) {
        if (build_json_payload(&ipc->cache, ipc->session_start_ts, payload,
                                 sizeof(payload)) != 0) {
            (void)close(client_fd);
            return;
        }
        out = payload;
        out_len = strlen(payload);
    } else if (header == IPC_HEADER_ASCII) {
        if (build_ascii_payload(&ipc->cache, payload, sizeof(payload)) != 0) {
            (void)close(client_fd);
            return;
        }
        out = payload;
        out_len = strlen(payload);
    } else {
        (void)close(client_fd);
        return;
    }

    nwritten = write(client_fd, out, out_len);
    (void)nwritten;
    (void)close(client_fd);
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
    ipc->session_start_ts = 0U;

    if (bind_listen_socket(&ipc->listen_fd) != 0) {
        free(ipc);
        return -1;
    }

    *out = ipc;
    return 0;
}

void ipc_socket_update_cache(IpcSocket *ipc, const struct PowerLedgerEvent *event)
{
    ledger_event_t type;

    if (ipc == NULL || event == NULL) {
        return;
    }

    memcpy(&ipc->cache, event, sizeof(ipc->cache));
    ipc->cache_valid = 1;

    type = (ledger_event_t)event->type;
    if (type == EV_WAKE || type == EV_UNPLUG) {
        ipc->session_start_ts = event->timestamp;
    }
}

int ipc_socket_dispatch(IpcSocket *ipc)
{
    int client_fd;

    if (ipc == NULL || ipc->listen_fd < 0) {
        errno = EINVAL;
        return -1;
    }

    for (;;) {
        client_fd = accept(ipc->listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        (void)set_nonblocking(client_fd);
        service_client(ipc, client_fd);
    }
}

void ipc_socket_shutdown(IpcSocket *ipc, int epfd)
{
    if (ipc == NULL) {
        return;
    }

    if (ipc->listen_fd >= 0) {
        if (epfd >= 0) {
            (void)epoll_ctl(epfd, EPOLL_CTL_DEL, ipc->listen_fd, NULL);
        }
        (void)close(ipc->listen_fd);
        ipc->listen_fd = -1;
    }

    (void)unlink(IPC_SOCKET_PATH);
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
