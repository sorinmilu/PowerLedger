#ifdef TEST_MODE

#define _POSIX_C_SOURCE 200809L

#include "binary_io.h"
#include "hardware_mock.h"
#include "ledger_format.h"
#include "sysfs_poll.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_LEDGER_PATH "tmp/test_runner_ledger.bin"

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

int main(void)
{
    int rc = EXIT_SUCCESS;

    if (test_mock_sample() != 0) {
        rc = EXIT_FAILURE;
    }

    if (test_debounce() != 0) {
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
