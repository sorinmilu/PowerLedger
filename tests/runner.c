#ifdef TEST_MODE

#define _POSIX_C_SOURCE 200809L

#include "binary_io.h"
#include "hardware_mock.h"
#include "ledger_format.h"
#include "sysfs_poll.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_LEDGER_PATH        "tmp/test_runner_ledger.bin"
#define TEST_RIEMANN_PATH       "tmp/test_riemann_zero.bin"
#define BAT_TIME_BIN            "./bin/bat-time"
#define FOUR_HOURS_SECONDS      (4U * 3600U)

static int count_ledger_events(const char *path, size_t *out_count)
{
    struct stat st;

    if (out_count == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (stat(path, &st) != 0) {
        return -1;
    }

    if (st.st_size % (off_t)sizeof(struct PowerLedgerEvent) != 0) {
        errno = EIO;
        return -1;
    }

    *out_count = (size_t)st.st_size / sizeof(struct PowerLedgerEvent);
    return 0;
}

static int prepare_test_ledger(void)
{
    if (mkdir("tmp", 0700) != 0 && errno != EEXIST) {
        return -1;
    }

    if (unlink(TEST_LEDGER_PATH) != 0 && errno != ENOENT) {
        return -1;
    }

    return 0;
}

static int test_debounce(void)
{
    BinaryIo *io = NULL;
    struct SysfsSample sample;
    struct PowerLedgerEvent event;
    size_t events_logged = 0U;
    size_t on_disk = 0U;
    int i;

    if (hardware_mock_init() != 0) {
        perror("hardware_mock_init");
        return -1;
    }

    if (prepare_test_ledger() != 0) {
        perror("prepare_test_ledger");
        return -1;
    }

    if (binary_io_open(&io, TEST_LEDGER_PATH) != 0) {
        perror("binary_io_open");
        return -1;
    }

    sysfs_poll_ac_debounce_reset();

    for (i = 0; i < 5; i++) {
        ledger_event_t ev = EV_UNPLUG;

        if (hardware_mock_set_ac_online(i % 2) != 0) {
            perror("hardware_mock_set_ac_online");
            binary_io_close(io);
            return -1;
        }

        if (!sysfs_poll_ac_debounce_accept(ev)) {
            continue;
        }

        if (sysfs_poll_sample(&sample) != 0) {
            perror("sysfs_poll_sample");
            binary_io_close(io);
            return -1;
        }

        sysfs_poll_build_event(ev, &sample, &event);
        if (binary_io_append(io, &event) != 0) {
            perror("binary_io_append");
            binary_io_close(io);
            return -1;
        }

        events_logged++;
    }

    binary_io_close(io);
    io = NULL;

    if (count_ledger_events(TEST_LEDGER_PATH, &on_disk) != 0) {
        perror("count_ledger_events");
        return -1;
    }

    if (events_logged != 1U || on_disk != 1U) {
        fprintf(stderr,
                "debounce test failed: expected 1 event, logged %zu on-disk %zu\n",
                events_logged,
                on_disk);
        return -1;
    }

    printf("debounce test passed: %zu event written\n", on_disk);
    return 0;
}

static int test_mock_sample(void)
{
    struct SysfsSample sample;

    if (hardware_mock_init() != 0) {
        perror("hardware_mock_init");
        return -1;
    }

    if (sysfs_poll_sample(&sample) != 0) {
        perror("sysfs_poll_sample");
        return -1;
    }

    if (sample.power_drain >= 0) {
        fprintf(stderr, "mock sample failed: expected negative discharge power\n");
        return -1;
    }

    if (sample.fan_speed == 0U) {
        fprintf(stderr, "mock sample failed: fan speed is zero\n");
        return -1;
    }

    printf("mock sample test passed: power=%d fan=%u regime=%u\n",
           sample.power_drain,
           (unsigned)sample.fan_speed,
           (unsigned)sample.power_regime);
    return 0;
}

static int test_riemann_zero_volume(void)
{
    BinaryIo *io = NULL;
    struct PowerLedgerEvent events[2];
    FILE *fp;
    char line[256];
    double energy_wh = -1.0;
    const int32_t load_uw = -15000000;
    const uint32_t base_ts = 1000U;

    if (mkdir("tmp", 0700) != 0 && errno != EEXIST) {
        perror("mkdir tmp");
        return -1;
    }

    if (unlink(TEST_RIEMANN_PATH) != 0 && errno != ENOENT) {
        perror("unlink TEST_RIEMANN_PATH");
        return -1;
    }

    memset(events, 0, sizeof(events));

    events[0].type = (uint8_t)EV_SLEEP;
    events[0].power_drain = load_uw;
    events[0].timestamp = base_ts;

    events[1].type = (uint8_t)EV_TICK;
    events[1].power_drain = load_uw;
    events[1].timestamp = base_ts + FOUR_HOURS_SECONDS;

    if (binary_io_open(&io, TEST_RIEMANN_PATH) != 0) {
        perror("binary_io_open riemann");
        return -1;
    }

    if (binary_io_append(io, &events[0]) != 0 || binary_io_append(io, &events[1]) != 0) {
        perror("binary_io_append riemann");
        binary_io_close(io);
        return -1;
    }

    binary_io_close(io);
    io = NULL;

    fp = popen(BAT_TIME_BIN " -f " TEST_RIEMANN_PATH " --all 2>&1", "r");
    if (fp == NULL) {
        perror("popen bat-time");
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        if (sscanf(line, "Net Energy Consumed    : %lf Wh", &energy_wh) == 1) {
            break;
        }
    }

    if (pclose(fp) == -1) {
        perror("pclose bat-time");
        return -1;
    }

    if (energy_wh < 0.0) {
        fprintf(stderr, "riemann zero-volume test failed: bat-time output missing energy line\n");
        return -1;
    }

    if (fabs(energy_wh) > 0.005) {
        fprintf(stderr,
                "riemann zero-volume test failed: expected 0.00 Wh, got %.4f\n",
                energy_wh);
        return -1;
    }

    printf("riemann zero-volume test passed: %.2f Wh over 4h EV_SLEEP gap\n", energy_wh);
    return 0;
}

int main(void)
{
    int rc = EXIT_SUCCESS;

    if (test_mock_sample() != 0) {
        rc = EXIT_FAILURE;
    }

    if (test_debounce() != 0) {
        rc = EXIT_FAILURE;
    }

    if (test_riemann_zero_volume() != 0) {
        rc = EXIT_FAILURE;
    }

    hardware_mock_cleanup();
    return rc;
}

#else

int main(void)
{
    return 0;
}

#endif /* TEST_MODE */
