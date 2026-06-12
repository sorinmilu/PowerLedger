#ifdef TEST_MODE

#define _POSIX_C_SOURCE 200809L

#include "hardware_mock.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MOCK_ROOT "mock_sys"

static int write_mock_file(const char *relative_path, const char *value)
{
    char path[512];
    FILE *fp;

    if (snprintf(path, sizeof(path), "%s/%s", MOCK_ROOT, relative_path) >= (int)sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    fp = fopen(path, "we");
    if (fp == NULL) {
        return -1;
    }

    if (fputs(value, fp) == EOF) {
        (void)fclose(fp);
        return -1;
    }

    if (fclose(fp) != 0) {
        return -1;
    }

    return 0;
}

static int ensure_dir(const char *relative_path)
{
    char path[512];
    char *slash;

    if (snprintf(path, sizeof(path), "%s/%s", MOCK_ROOT, relative_path) >= (int)sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (mkdir(MOCK_ROOT, 0755) != 0 && errno != EEXIST) {
        return -1;
    }

    slash = path + strlen(MOCK_ROOT) + 1U;
    while (*slash != '\0') {
        if (*slash == '/') {
            *slash = '\0';
            if (mkdir(path, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *slash = '/';
        }
        slash++;
    }

    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        return -1;
    }

    return 0;
}

int hardware_mock_init(void)
{
    if (ensure_dir("class/power_supply/BAT0") != 0 ||
        ensure_dir("class/power_supply/AC0") != 0 ||
        ensure_dir("class/hwmon/hwmon0") != 0 ||
        ensure_dir("devices/system/cpu/cpu0/cpufreq") != 0) {
        return -1;
    }

    if (write_mock_file("class/power_supply/BAT0/power_now", "15000000\n") != 0 ||
        write_mock_file("class/power_supply/BAT0/status", "Discharging\n") != 0 ||
        write_mock_file("class/power_supply/BAT0/capacity", "72\n") != 0 ||
        write_mock_file("class/power_supply/AC0/online", "1\n") != 0 ||
        write_mock_file("class/hwmon/hwmon0/name", "asus\n") != 0 ||
        write_mock_file("class/hwmon/hwmon0/fan1_input", "2400\n") != 0 ||
        write_mock_file("devices/system/cpu/cpu0/cpufreq/energy_performance_preference",
                        "balanced\n") != 0) {
        return -1;
    }

    return 0;
}

int hardware_mock_set_ac_online(int online)
{
    return write_mock_file("class/power_supply/AC0/online", online ? "1\n" : "0\n");
}

void hardware_mock_cleanup(void)
{
    (void)system("rm -rf mock_sys tmp");
}

#endif /* TEST_MODE */
