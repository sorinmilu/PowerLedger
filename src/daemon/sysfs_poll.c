#define _POSIX_C_SOURCE 200809L

#include "sysfs_poll.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define HWMON_SCAN_MAX 32
#define ETA_MAX_MINS (7L * 24L * 60L)

static uint64_t last_ac_event_us;
static ledger_event_t last_ac_event_type;
static int last_ac_event_valid;

const char *sysfs_poll_resolve_path(const char *sys_path)
{
#ifdef TEST_MODE
    static char resolved[512];

    if (sys_path == NULL) {
        return NULL;
    }

    if (strncmp(sys_path, "/sys/", 5) == 0) {
        if (snprintf(resolved, sizeof(resolved), "./mock_sys/%s", sys_path + 5) >=
            (int)sizeof(resolved)) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        return resolved;
    }
#endif

    return sys_path;
}

static void strip_line(char *buf)
{
    size_t len;

    if (buf == NULL) {
        return;
    }

    len = strlen(buf);
    while (len > 0U && (buf[len - 1U] == '\n' || buf[len - 1U] == '\r')) {
        buf[len - 1U] = '\0';
        len--;
    }
}

static int read_sysfs_text(const char *sys_path, char *buf, size_t buflen)
{
    const char *path;
    int fd;
    ssize_t nread;

    if (sys_path == NULL || buf == NULL || buflen == 0U) {
        errno = EINVAL;
        return -1;
    }

    path = sysfs_poll_resolve_path(sys_path);
    if (path == NULL) {
        return -1;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    nread = read(fd, buf, buflen - 1U);
    (void)close(fd);

    if (nread <= 0) {
        return -1;
    }

    buf[(size_t)nread] = '\0';
    strip_line(buf);
    return 0;
}

static int parse_sysfs_int(const char *sys_path, long *out)
{
    char buf[64];
    char *end;
    long value;

    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (read_sysfs_text(sys_path, buf, sizeof(buf)) != 0) {
        return -1;
    }

    errno = 0;
    value = strtol(buf, &end, 10);
    if (errno != 0 || end == buf) {
        errno = EINVAL;
        return -1;
    }

    *out = value;
    return 0;
}

static uint8_t map_power_regime(const char *text)
{
    if (text == NULL) {
        return (uint8_t)REG_BALANCED;
    }

    if (strcmp(text, "performance") == 0 || strcmp(text, "default") == 0) {
        return (uint8_t)REG_PERFORMANCE;
    }
    if (strcmp(text, "balanced") == 0 || strcmp(text, "balance_performance") == 0) {
        return (uint8_t)REG_BALANCED;
    }
    if (strcmp(text, "power") == 0 || strcmp(text, "balance_power") == 0 ||
        strcmp(text, "power-save") == 0) {
        return (uint8_t)REG_POWER_SAVE;
    }
    if (strcmp(text, "quiet") == 0 || strcmp(text, "low-power") == 0) {
        return (uint8_t)REG_QUIET;
    }

    return (uint8_t)REG_BALANCED;
}

static int read_power_regime(uint8_t *out)
{
    char buf[64];

    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (read_sysfs_text(
            "/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference",
            buf, sizeof(buf)) == 0) {
        *out = map_power_regime(buf);
        return 0;
    }

    if (read_sysfs_text("/sys/firmware/acpi/platform_profile", buf, sizeof(buf)) == 0) {
        *out = map_power_regime(buf);
        return 0;
    }

    *out = (uint8_t)REG_BALANCED;
    return 0;
}

static int read_fan_speed(uint16_t *out)
{
    char name_buf[64];
    char fan_path[256];
    long rpm;
    int i;

    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }

    for (i = 0; i < HWMON_SCAN_MAX; i++) {
        if (snprintf(fan_path, sizeof(fan_path), "/sys/class/hwmon/hwmon%d/name", i) >=
            (int)sizeof(fan_path)) {
            continue;
        }

        if (read_sysfs_text(fan_path, name_buf, sizeof(name_buf)) != 0) {
            continue;
        }

        if (strcmp(name_buf, "asus") != 0 && strcmp(name_buf, "coretemp") != 0) {
            continue;
        }

        if (snprintf(fan_path, sizeof(fan_path), "/sys/class/hwmon/hwmon%d/fan1_input", i) >=
            (int)sizeof(fan_path)) {
            return -1;
        }

        if (parse_sysfs_int(fan_path, &rpm) != 0) {
            return -1;
        }

        if (rpm < 0) {
            rpm = 0;
        }
        if (rpm > 65535L) {
            rpm = 65535L;
        }

        *out = (uint16_t)rpm;
        return 0;
    }

    *out = 0U;
    return 0;
}

static int read_battery_power(int32_t *out)
{
    char status[32];
    long power_uw;

    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (parse_sysfs_int("/sys/class/power_supply/BAT0/power_now", &power_uw) != 0) {
        return -1;
    }

    if (read_sysfs_text("/sys/class/power_supply/BAT0/status", status, sizeof(status)) == 0 &&
        strcmp(status, "Discharging") == 0) {
        power_uw = -power_uw;
    }

    *out = (int32_t)power_uw;
    return 0;
}

static int read_ac_online_sysfs(uint8_t *out)
{
    static const char *const ac_paths[] = {
        "/sys/class/power_supply/AC0/online",
        "/sys/class/power_supply/ADP1/online",
        NULL,
    };
    long value;
    size_t i;

    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }

    for (i = 0; ac_paths[i] != NULL; i++) {
        if (parse_sysfs_int(ac_paths[i], &value) == 0) {
            *out = (value != 0) ? 1U : 0U;
            return 0;
        }
    }

    *out = 0U;
    return 0;
}

static int eta_mins_valid(long mins)
{
    return mins > 0L && mins <= ETA_MAX_MINS;
}

static int read_time_to_empty_mins(long *out_mins)
{
    long energy_now;
    long power_uw;

    if (out_mins == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (parse_sysfs_int("/sys/class/power_supply/BAT0/time_to_empty_now", out_mins) == 0 &&
        eta_mins_valid(*out_mins)) {
        return 0;
    }

    if (parse_sysfs_int("/sys/class/power_supply/BAT0/energy_now", &energy_now) != 0) {
        return -1;
    }
    if (parse_sysfs_int("/sys/class/power_supply/BAT0/power_now", &power_uw) != 0 ||
        power_uw <= 0L) {
        return -1;
    }

    *out_mins = (long)((double)energy_now / (double)power_uw * 60.0);
    if (!eta_mins_valid(*out_mins)) {
        return -1;
    }

    return 0;
}

static int read_time_to_full_mins(long *out_mins)
{
    long energy_now;
    long energy_full;
    long power_uw;

    if (out_mins == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (parse_sysfs_int("/sys/class/power_supply/BAT0/time_to_full_now", out_mins) == 0 &&
        eta_mins_valid(*out_mins)) {
        return 0;
    }

    if (parse_sysfs_int("/sys/class/power_supply/BAT0/energy_now", &energy_now) != 0 ||
        parse_sysfs_int("/sys/class/power_supply/BAT0/energy_full", &energy_full) != 0) {
        return -1;
    }
    if (parse_sysfs_int("/sys/class/power_supply/BAT0/power_now", &power_uw) != 0 ||
        power_uw <= 0L) {
        return -1;
    }
    if (energy_full <= energy_now) {
        return -1;
    }

    *out_mins = (long)((double)(energy_full - energy_now) / (double)power_uw * 60.0);
    if (!eta_mins_valid(*out_mins)) {
        return -1;
    }

    return 0;
}

static void populate_battery_etas(struct SysfsSample *sample)
{
    char status[32];
    long eta_mins;

    if (sample == NULL) {
        return;
    }

    sample->remaining_mins = SYSFS_ETA_NA;
    sample->to_full_mins = SYSFS_ETA_NA;

    if (read_sysfs_text("/sys/class/power_supply/BAT0/status", status, sizeof(status)) != 0) {
        status[0] = '\0';
    }

    if (!sample->ac_online &&
        (strcmp(status, "Discharging") == 0 || sample->power_drain < 0)) {
        if (read_time_to_empty_mins(&eta_mins) == 0) {
            sample->remaining_mins = (int32_t)eta_mins;
        }
    }

    if (sample->ac_online && strcmp(status, "Charging") == 0) {
        if (read_time_to_full_mins(&eta_mins) == 0) {
            sample->to_full_mins = (int32_t)eta_mins;
        }
    }
}

static int read_battery_level(uint8_t *out)
{
    long level;

    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (parse_sysfs_int("/sys/class/power_supply/BAT0/capacity", &level) != 0) {
        *out = 0U;
        return 0;
    }

    if (level < 0) {
        level = 0;
    }
    if (level > 100) {
        level = 100;
    }

    *out = (uint8_t)level;
    return 0;
}

static uint32_t monotonic_raw_seconds(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
        return 0U;
    }

    return (uint32_t)ts.tv_sec;
}

int sysfs_poll_open_ac_fd(void)
{
    static const char *const ac_paths[] = {
        "/sys/class/power_supply/AC0/online",
        "/sys/class/power_supply/ADP1/online",
        NULL,
    };
    size_t i;
    const char *resolved;
    int fd;
    int flags;

    for (i = 0; ac_paths[i] != NULL; i++) {
        resolved = sysfs_poll_resolve_path(ac_paths[i]);
        if (resolved == NULL) {
            continue;
        }

        fd = open(resolved, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            flags = fcntl(fd, F_GETFL);
            if (flags >= 0) {
                (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            }
            return fd;
        }
    }

    errno = ENOENT;
    return -1;
}

int sysfs_poll_read_ac_online(int ac_fd)
{
    char buf[16];
    ssize_t nread;

    if (ac_fd < 0) {
        errno = EINVAL;
        return -1;
    }

    if (lseek(ac_fd, 0, SEEK_SET) < 0) {
        return -1;
    }

    nread = read(ac_fd, buf, sizeof(buf) - 1U);
    if (nread <= 0) {
        return -1;
    }

    buf[(size_t)nread] = '\0';
    strip_line(buf);
    return (buf[0] == '1') ? 1 : 0;
}

void sysfs_poll_ac_debounce_reset(void)
{
    last_ac_event_us = 0U;
    last_ac_event_type = EV_PLUG;
    last_ac_event_valid = 0;
}

int sysfs_poll_ac_debounce_accept(ledger_event_t ev)
{
    struct timespec ts;
    uint64_t now_us;

    if (ev != EV_PLUG && ev != EV_UNPLUG) {
        errno = EINVAL;
        return 0;
    }

    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
        return 0;
    }

    now_us = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;

    if (last_ac_event_valid && ev == last_ac_event_type &&
        now_us - last_ac_event_us < 3000000ULL) {
        return 0;
    }

    last_ac_event_us = now_us;
    last_ac_event_type = ev;
    last_ac_event_valid = 1;
    return 1;
}

int sysfs_poll_sample(struct SysfsSample *out)
{
    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->remaining_mins = SYSFS_ETA_NA;
    out->to_full_mins = SYSFS_ETA_NA;
    out->timestamp = monotonic_raw_seconds();

    if (read_battery_power(&out->power_drain) != 0) {
        return -1;
    }

    (void)read_battery_level(&out->battery_level);
    (void)read_fan_speed(&out->fan_speed);
    (void)read_power_regime(&out->power_regime);
    (void)read_ac_online_sysfs(&out->ac_online);
    populate_battery_etas(out);
    return 0;
}

void sysfs_poll_build_event(ledger_event_t type, const struct SysfsSample *sample,
                            struct PowerLedgerEvent *out)
{
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->type = (uint8_t)type;

    if (sample != NULL) {
        out->battery_level = sample->battery_level;
        out->fan_speed = sample->fan_speed;
        out->power_drain = sample->power_drain;
        out->timestamp = sample->timestamp;
        out->power_regime = sample->power_regime;
    }
}
