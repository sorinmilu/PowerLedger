#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "binary_io.h"
#include "ledger_format.h"

#define TEST_RECORD_COUNT 100
#define TEST_LEDGER_PATH  "tmp/test_ledger.bin"
#define EXPECTED_SIZE     (TEST_RECORD_COUNT * sizeof(struct PowerLedgerEvent))

static int prepare_test_path(void)
{
    if (mkdir("tmp", 0700) != 0 && errno != EEXIST) {
        return -1;
    }

    if (unlink(TEST_LEDGER_PATH) != 0 && errno != ENOENT) {
        return -1;
    }

    return 0;
}

static void fill_dummy_event(struct PowerLedgerEvent *event, size_t index)
{
    memset(event, 0, sizeof(*event));
    event->type = (uint8_t)((index % 5U) + 1U);
    event->battery_level = (uint8_t)(index % 101U);
    event->fan_speed = (uint16_t)(1800U + (index % 500U));
    event->power_drain = (index % 2U == 0U) ? -15000000 : 8000000;
    event->timestamp = (uint32_t)(1000U + index);
    event->power_regime = (uint8_t)((index % 4U) + 1U);
}

int main(void)
{
    BinaryIo *io = NULL;
    struct stat st;
    size_t i;

    if (prepare_test_path() != 0) {
        perror("prepare_test_path");
        return EXIT_FAILURE;
    }

    if (binary_io_open(&io, TEST_LEDGER_PATH) != 0) {
        perror("binary_io_open");
        return EXIT_FAILURE;
    }

    for (i = 0; i < TEST_RECORD_COUNT; i++) {
        struct PowerLedgerEvent event;

        fill_dummy_event(&event, i);
        if (binary_io_append(io, &event) != 0) {
            perror("binary_io_append");
            binary_io_close(io);
            return EXIT_FAILURE;
        }
    }

    binary_io_close(io);
    io = NULL;

    if (stat(TEST_LEDGER_PATH, &st) != 0) {
        perror("stat");
        return EXIT_FAILURE;
    }

    if ((size_t)st.st_size != EXPECTED_SIZE) {
        fprintf(stderr,
                "layout test failed: expected %zu bytes, got %ld bytes\n",
                EXPECTED_SIZE,
                (long)st.st_size);
        return EXIT_FAILURE;
    }

    printf("layout test passed: %zu records, %ld bytes\n",
           (size_t)TEST_RECORD_COUNT,
           (long)st.st_size);
    return EXIT_SUCCESS;
}
