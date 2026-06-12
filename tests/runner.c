#ifdef TEST_MODE

#define _POSIX_C_SOURCE 200809L

#include "binary_io.h"
#include "duration_format.h"
#include "hardware_mock.h"
#include "ipc_socket.h"
#include "ledger_format.h"
#include "sysfs_poll.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
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

static int test_duration_format(void)
{
    char buf[32];

    duration_format_mins(42, buf, sizeof(buf));
    if (strcmp(buf, "42m") != 0) {
        fprintf(stderr, "duration_format test failed: expected 42m, got %s\n", buf);
        return -1;
    }

    duration_format_mins(84, buf, sizeof(buf));
    if (strcmp(buf, "1h 24m") != 0) {
        fprintf(stderr, "duration_format test failed: expected 1h 24m, got %s\n", buf);
        return -1;
    }

    printf("duration_format test passed\n");
    return 0;
}

static int test_sysfs_battery_eta_discharge(void)
{
    struct SysfsSample sample;

    if (hardware_mock_init() != 0) {
        perror("hardware_mock_init");
        return -1;
    }

    if (hardware_mock_set_ac_online(0) != 0 ||
        hardware_mock_set_bat_status("Discharging") != 0 ||
        hardware_mock_set_bat_power(15000000L) != 0 ||
        hardware_mock_set_bat_energy(36000000L, 50000000L) != 0) {
        perror("hardware_mock discharge setup");
        return -1;
    }

    if (sysfs_poll_sample(&sample) != 0) {
        perror("sysfs_poll_sample");
        return -1;
    }

    if (sample.remaining_mins != 144) {
        fprintf(stderr,
                "sysfs ETA discharge test failed: expected 144 min, got %d\n",
                sample.remaining_mins);
        return -1;
    }

    if (sample.to_full_mins != SYSFS_ETA_NA) {
        fprintf(stderr, "sysfs ETA discharge test failed: to_full should be N/A\n");
        return -1;
    }

    printf("sysfs ETA discharge test passed: remaining_mins=%d\n", sample.remaining_mins);
    return 0;
}

static int test_sysfs_battery_eta_charge(void)
{
    struct SysfsSample sample;

    if (hardware_mock_init() != 0) {
        perror("hardware_mock_init");
        return -1;
    }

    if (hardware_mock_set_ac_online(1) != 0 ||
        hardware_mock_set_bat_status("Charging") != 0 ||
        hardware_mock_set_bat_power(10000000L) != 0 ||
        hardware_mock_set_bat_energy(36000000L, 50000000L) != 0) {
        perror("hardware_mock charge setup");
        return -1;
    }

    if (sysfs_poll_sample(&sample) != 0) {
        perror("sysfs_poll_sample");
        return -1;
    }

    if (sample.to_full_mins != 84) {
        fprintf(stderr,
                "sysfs ETA charge test failed: expected 84 min, got %d\n",
                sample.to_full_mins);
        return -1;
    }

    if (sample.remaining_mins != SYSFS_ETA_NA) {
        fprintf(stderr, "sysfs ETA charge test failed: remaining should be N/A\n");
        return -1;
    }

    printf("sysfs ETA charge test passed: to_full_mins=%d\n", sample.to_full_mins);
    return 0;
}

static int test_sysfs_battery_eta_full(void)
{
    struct SysfsSample sample;

    if (hardware_mock_init() != 0) {
        perror("hardware_mock_init");
        return -1;
    }

    if (hardware_mock_set_ac_online(1) != 0 ||
        hardware_mock_set_bat_status("Full") != 0 ||
        hardware_mock_set_bat_power(0L) != 0) {
        perror("hardware_mock full setup");
        return -1;
    }

    if (sysfs_poll_sample(&sample) != 0) {
        perror("sysfs_poll_sample");
        return -1;
    }

    if (sample.remaining_mins != SYSFS_ETA_NA || sample.to_full_mins != SYSFS_ETA_NA) {
        fprintf(stderr, "sysfs ETA full test failed: expected both ETA fields N/A\n");
        return -1;
    }

    printf("sysfs ETA full test passed\n");
    return 0;
}

static int test_session_wake_not_boundary(void)
{
    BinaryIo *io = NULL;
    struct PowerLedgerEvent events[5];
    uint32_t session_seconds = 0U;
    uint32_t all_seconds = 0U;

    if (mkdir("tmp", 0700) != 0 && errno != EEXIST) {
        return -1;
    }

    if (unlink("tmp/test_wake_not_boundary.bin") != 0 && errno != ENOENT) {
        return -1;
    }

    memset(events, 0, sizeof(events));
    events[0].type = (uint8_t)EV_TICK;
    events[0].timestamp = 1000U;
    events[1].type = (uint8_t)EV_TICK;
    events[1].timestamp = 1060U;
    events[2].type = (uint8_t)EV_SLEEP;
    events[2].timestamp = 1060U;
    events[3].type = (uint8_t)EV_WAKE;
    events[3].timestamp = 87460U;
    events[4].type = (uint8_t)EV_TICK;
    events[4].timestamp = 87520U;

    if (binary_io_open(&io, "tmp/test_wake_not_boundary.bin") != 0) {
        perror("binary_io_open wake boundary");
        return -1;
    }

    if (binary_io_append(io, &events[0]) != 0 ||
        binary_io_append(io, &events[1]) != 0 ||
        binary_io_append(io, &events[2]) != 0 ||
        binary_io_append(io, &events[3]) != 0 ||
        binary_io_append(io, &events[4]) != 0) {
        perror("binary_io_append wake boundary");
        binary_io_close(io);
        return -1;
    }

    binary_io_close(io);
    io = NULL;

    if (binary_io_compute_session_active("tmp/test_wake_not_boundary.bin", 0,
                                         &session_seconds, NULL, NULL) != 0) {
        perror("binary_io_compute_session_active wake boundary");
        return -1;
    }

    if (binary_io_compute_session_active("tmp/test_wake_not_boundary.bin", 1,
                                         &all_seconds, NULL, NULL) != 0) {
        perror("binary_io_compute_session_active wake all");
        return -1;
    }

    if (session_seconds != all_seconds) {
        fprintf(stderr,
                "wake boundary test failed: session=%u all=%u (expected equal)\n",
                session_seconds, all_seconds);
        return -1;
    }

    if (session_seconds != 120U) {
        fprintf(stderr,
                "wake boundary test failed: expected 120 active seconds, got %u\n",
                session_seconds);
        return -1;
    }

    printf("session wake-not-boundary test passed: %u seconds\n", session_seconds);
    return 0;
}

static int test_session_terminal_unplug_ignored(void)
{
    BinaryIo *io = NULL;
    struct PowerLedgerEvent events[3];
    uint32_t session_seconds = 0U;

    if (mkdir("tmp", 0700) != 0 && errno != EEXIST) {
        return -1;
    }

    if (unlink("tmp/test_terminal_unplug.bin") != 0 && errno != ENOENT) {
        return -1;
    }

    memset(events, 0, sizeof(events));
    events[0].type = (uint8_t)EV_TICK;
    events[0].timestamp = 1000U;
    events[1].type = (uint8_t)EV_TICK;
    events[1].timestamp = 1060U;
    events[2].type = (uint8_t)EV_UNPLUG;
    events[2].timestamp = 1120U;

    if (binary_io_open(&io, "tmp/test_terminal_unplug.bin") != 0) {
        perror("binary_io_open terminal unplug");
        return -1;
    }

    if (binary_io_append(io, &events[0]) != 0 ||
        binary_io_append(io, &events[1]) != 0 ||
        binary_io_append(io, &events[2]) != 0) {
        perror("binary_io_append terminal unplug");
        binary_io_close(io);
        return -1;
    }

    binary_io_close(io);
    io = NULL;

    if (binary_io_compute_session_active("tmp/test_terminal_unplug.bin", 0,
                                         &session_seconds, NULL, NULL) != 0) {
        perror("binary_io_compute_session_active terminal unplug");
        return -1;
    }

    if (session_seconds != 120U) {
        fprintf(stderr,
                "terminal unplug test failed: expected 120 seconds, got %u\n",
                session_seconds);
        return -1;
    }

    printf("session terminal unplug ignored test passed: %u seconds\n", session_seconds);
    return 0;
}

static int test_discharge_stops_at_plug(void)
{
    BinaryIo *io = NULL;
    struct PowerLedgerEvent events[5];
    struct BinaryIoDischargeResult discharge;

    if (mkdir("tmp", 0700) != 0 && errno != EEXIST) {
        return -1;
    }

    if (unlink("tmp/test_discharge_plug.bin") != 0 && errno != ENOENT) {
        return -1;
    }

    memset(events, 0, sizeof(events));
    events[0].type = (uint8_t)EV_PLUG;
    events[0].timestamp = 1000U;
    events[1].type = (uint8_t)EV_UNPLUG;
    events[1].timestamp = 1060U;
    events[2].type = (uint8_t)EV_TICK;
    events[2].timestamp = 1120U;
    events[3].type = (uint8_t)EV_PLUG;
    events[3].timestamp = 1180U;
    events[4].type = (uint8_t)EV_TICK;
    events[4].timestamp = 1240U;

    if (binary_io_open(&io, "tmp/test_discharge_plug.bin") != 0) {
        perror("binary_io_open discharge plug");
        return -1;
    }

    if (binary_io_append(io, &events[0]) != 0 ||
        binary_io_append(io, &events[1]) != 0 ||
        binary_io_append(io, &events[2]) != 0 ||
        binary_io_append(io, &events[3]) != 0 ||
        binary_io_append(io, &events[4]) != 0) {
        perror("binary_io_append discharge plug");
        binary_io_close(io);
        return -1;
    }

    binary_io_close(io);
    io = NULL;

    if (binary_io_analyze_discharge("tmp/test_discharge_plug.bin", 0, &discharge) != 0) {
        perror("binary_io_analyze_discharge plug stop");
        return -1;
    }

    if (discharge.active_seconds != 120U) {
        fprintf(stderr,
                "discharge plug-stop test failed: expected 120 seconds, got %u\n",
                discharge.active_seconds);
        return -1;
    }

    printf("discharge plug-stop test passed: %u seconds\n", discharge.active_seconds);
    return 0;
}

static int test_discharge_spurious_unplug_ignored(void)
{
    BinaryIo *io = NULL;
    struct PowerLedgerEvent events[5];
    struct BinaryIoDischargeResult discharge;

    if (mkdir("tmp", 0700) != 0 && errno != EEXIST) {
        return -1;
    }

    if (unlink("tmp/test_spurious_unplug.bin") != 0 && errno != ENOENT) {
        return -1;
    }

    memset(events, 0, sizeof(events));
    events[0].type = (uint8_t)EV_TICK;
    events[0].timestamp = 1000U;
    events[1].type = (uint8_t)EV_TICK;
    events[1].timestamp = 1060U;
    events[2].type = (uint8_t)EV_UNPLUG;
    events[2].timestamp = 1120U;
    events[3].type = (uint8_t)EV_TICK;
    events[3].timestamp = 1180U;
    events[4].type = (uint8_t)EV_TICK;
    events[4].timestamp = 1240U;

    if (binary_io_open(&io, "tmp/test_spurious_unplug.bin") != 0) {
        perror("binary_io_open spurious unplug");
        return -1;
    }

    if (binary_io_append(io, &events[0]) != 0 ||
        binary_io_append(io, &events[1]) != 0 ||
        binary_io_append(io, &events[2]) != 0 ||
        binary_io_append(io, &events[3]) != 0 ||
        binary_io_append(io, &events[4]) != 0) {
        perror("binary_io_append spurious unplug");
        binary_io_close(io);
        return -1;
    }

    binary_io_close(io);
    io = NULL;

    if (binary_io_analyze_discharge("tmp/test_spurious_unplug.bin", 0, &discharge) != 0) {
        perror("binary_io_analyze_discharge spurious unplug");
        return -1;
    }

    if (discharge.active_seconds != 240U) {
        fprintf(stderr,
                "spurious unplug test failed: expected 240 seconds, got %u\n",
                discharge.active_seconds);
        return -1;
    }

    printf("discharge spurious unplug ignored test passed: %u seconds\n",
           discharge.active_seconds);
    return 0;
}

static int test_discharge_on_ac_zero(void)
{
    BinaryIo *io = NULL;
    struct PowerLedgerEvent events[3];
    struct BinaryIoDischargeResult discharge;

    if (mkdir("tmp", 0700) != 0 && errno != EEXIST) {
        return -1;
    }

    if (unlink("tmp/test_discharge_on_ac.bin") != 0 && errno != ENOENT) {
        return -1;
    }

    memset(events, 0, sizeof(events));
    events[0].type = (uint8_t)EV_UNPLUG;
    events[0].timestamp = 1000U;
    events[1].type = (uint8_t)EV_TICK;
    events[1].timestamp = 1060U;
    events[2].type = (uint8_t)EV_TICK;
    events[2].timestamp = 1120U;

    if (binary_io_open(&io, "tmp/test_discharge_on_ac.bin") != 0) {
        perror("binary_io_open discharge on ac");
        return -1;
    }

    if (binary_io_append(io, &events[0]) != 0 ||
        binary_io_append(io, &events[1]) != 0 ||
        binary_io_append(io, &events[2]) != 0) {
        perror("binary_io_append discharge on ac");
        binary_io_close(io);
        return -1;
    }

    binary_io_close(io);
    io = NULL;

    if (binary_io_analyze_discharge("tmp/test_discharge_on_ac.bin", 1, &discharge) != 0) {
        perror("binary_io_analyze_discharge on ac");
        return -1;
    }

    if (discharge.active_seconds != 0U) {
        fprintf(stderr,
                "discharge on-ac test failed: expected 0 seconds, got %u\n",
                discharge.active_seconds);
        return -1;
    }

    printf("discharge on-ac zero test passed\n");
    return 0;
}

static int test_ipc_session_mins_zero_on_ac(void)
{
    BinaryIo *io = NULL;
    IpcSocket *ipc = NULL;
    struct PowerLedgerEvent events[3];
    struct SysfsSample sample;
    struct PowerLedgerEvent event;
    char json[640];
    unsigned session_mins;

    if (mkdir("tmp", 0700) != 0 && errno != EEXIST) {
        return -1;
    }

    if (unlink("tmp/test_ipc_on_ac.bin") != 0 && errno != ENOENT) {
        return -1;
    }

    memset(events, 0, sizeof(events));
    events[0].type = (uint8_t)EV_UNPLUG;
    events[0].timestamp = 1000U;
    events[1].type = (uint8_t)EV_TICK;
    events[1].timestamp = 1060U;
    events[2].type = (uint8_t)EV_TICK;
    events[2].timestamp = 1120U;

    if (binary_io_open(&io, "tmp/test_ipc_on_ac.bin") != 0) {
        perror("binary_io_open ipc on ac");
        return -1;
    }

    if (binary_io_append(io, &events[0]) != 0 ||
        binary_io_append(io, &events[1]) != 0 ||
        binary_io_append(io, &events[2]) != 0) {
        perror("binary_io_append ipc on ac");
        binary_io_close(io);
        return -1;
    }

    binary_io_close(io);
    io = NULL;

    if (hardware_mock_init() != 0) {
        perror("hardware_mock_init");
        return -1;
    }

    if (hardware_mock_set_ac_online(1) != 0 ||
        hardware_mock_set_bat_status("Charging") != 0 ||
        hardware_mock_set_bat_power(10000000L) != 0) {
        perror("hardware_mock ipc on ac");
        return -1;
    }

    if (ipc_socket_init(&ipc) != 0) {
        perror("ipc_socket_init on ac");
        return -1;
    }

    ipc_socket_restore_session(ipc, "tmp/test_ipc_on_ac.bin");

    if (sysfs_poll_sample(&sample) != 0) {
        perror("sysfs_poll_sample on ac");
        ipc_socket_shutdown(ipc, -1);
        return -1;
    }

    sysfs_poll_build_event(EV_TICK, &sample, &event);
    ipc_socket_update_cache(ipc, &event, &sample, 0);

    if (ipc_socket_test_build_json(ipc, json, sizeof(json)) != 0) {
        perror("ipc_socket_test_build_json on ac");
        ipc_socket_shutdown(ipc, -1);
        return -1;
    }

    if (sscanf(strstr(json, "\"session_mins\":"), "\"session_mins\":%u", &session_mins) != 1) {
        fprintf(stderr, "ipc on-ac test failed: missing session_mins in %s", json);
        ipc_socket_shutdown(ipc, -1);
        return -1;
    }

    if (session_mins != 0U) {
        fprintf(stderr,
                "ipc on-ac test failed: expected session_mins=0, got %u in %s",
                session_mins, json);
        ipc_socket_shutdown(ipc, -1);
        return -1;
    }

    ipc_socket_shutdown(ipc, -1);
    printf("ipc on-ac session_mins zero test passed\n");
    return 0;
}

static int test_session_active_seconds(void)
{
    BinaryIo *io = NULL;
    struct PowerLedgerEvent events[3];
    uint32_t active_seconds = 0U;

    if (mkdir("tmp", 0700) != 0 && errno != EEXIST) {
        return -1;
    }

    if (unlink("tmp/test_session_active.bin") != 0 && errno != ENOENT) {
        return -1;
    }

    memset(events, 0, sizeof(events));
    events[0].type = (uint8_t)EV_UNPLUG;
    events[0].timestamp = 1000U;
    events[1].type = (uint8_t)EV_TICK;
    events[1].timestamp = 1060U;
    events[2].type = (uint8_t)EV_TICK;
    events[2].timestamp = 1120U;

    if (binary_io_open(&io, "tmp/test_session_active.bin") != 0) {
        perror("binary_io_open session active");
        return -1;
    }

    if (binary_io_append(io, &events[0]) != 0 ||
        binary_io_append(io, &events[1]) != 0 ||
        binary_io_append(io, &events[2]) != 0) {
        perror("binary_io_append session active");
        binary_io_close(io);
        return -1;
    }

    binary_io_close(io);
    io = NULL;

    if (binary_io_compute_session_active("tmp/test_session_active.bin", 0, &active_seconds,
                                         NULL, NULL) != 0) {
        perror("binary_io_compute_session_active");
        return -1;
    }

    if (active_seconds != 120U) {
        fprintf(stderr,
                "session active test failed: expected 120 seconds, got %u\n",
                active_seconds);
        return -1;
    }

    printf("session active seconds test passed: %u\n", active_seconds);
    return 0;
}

static int test_session_start_restore(void)
{
    BinaryIo *io = NULL;
    struct PowerLedgerEvent events[3];
    uint32_t start_ts = 0U;
    uint32_t now_ts = 5000U;

    if (mkdir("tmp", 0700) != 0 && errno != EEXIST) {
        return -1;
    }

    if (unlink("tmp/test_session_ledger.bin") != 0 && errno != ENOENT) {
        return -1;
    }

    memset(events, 0, sizeof(events));
    events[0].type = (uint8_t)EV_TICK;
    events[0].timestamp = 1000U;
    events[1].type = (uint8_t)EV_UNPLUG;
    events[1].timestamp = 2000U;
    events[2].type = (uint8_t)EV_TICK;
    events[2].timestamp = 4000U;

    if (binary_io_open(&io, "tmp/test_session_ledger.bin") != 0) {
        perror("binary_io_open session");
        return -1;
    }

    if (binary_io_append(io, &events[0]) != 0 ||
        binary_io_append(io, &events[1]) != 0 ||
        binary_io_append(io, &events[2]) != 0) {
        perror("binary_io_append session");
        binary_io_close(io);
        return -1;
    }

    binary_io_close(io);
    io = NULL;

    if (binary_io_find_session_start_ts("tmp/test_session_ledger.bin", now_ts, &start_ts) != 0) {
        perror("binary_io_find_session_start_ts");
        return -1;
    }

    if (start_ts != 2000U) {
        fprintf(stderr,
                "session start restore test failed: expected 2000, got %u\n",
                start_ts);
        return -1;
    }

    printf("session start restore test passed: ts=%u\n", start_ts);
    return 0;
}

static uint32_t test_monotonic_now_seconds(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
        return 0U;
    }

    return (uint32_t)ts.tv_sec;
}

static int test_ipc_session_mins_restore(void)
{
    BinaryIo *io = NULL;
    IpcSocket *ipc = NULL;
    struct PowerLedgerEvent events[4];
    struct SysfsSample sample;
    struct PowerLedgerEvent event;
    char json[640];
    uint32_t base_ts;
    unsigned session_mins;

    if (mkdir("tmp", 0700) != 0 && errno != EEXIST) {
        return -1;
    }

    if (unlink("tmp/test_ipc_session.bin") != 0 && errno != ENOENT) {
        return -1;
    }

    base_ts = test_monotonic_now_seconds();
    if (base_ts < 300U) {
        fprintf(stderr, "ipc session restore test skipped: monotonic clock too small\n");
        return 0;
    }
    base_ts -= 180U;

    memset(events, 0, sizeof(events));
    events[0].type = (uint8_t)EV_UNPLUG;
    events[0].timestamp = base_ts;
    events[1].type = (uint8_t)EV_TICK;
    events[1].timestamp = base_ts + 60U;
    events[2].type = (uint8_t)EV_TICK;
    events[2].timestamp = base_ts + 120U;
    events[3].type = (uint8_t)EV_TICK;
    events[3].timestamp = base_ts + 180U;

    if (binary_io_open(&io, "tmp/test_ipc_session.bin") != 0) {
        perror("binary_io_open ipc session");
        return -1;
    }

    if (binary_io_append(io, &events[0]) != 0 ||
        binary_io_append(io, &events[1]) != 0 ||
        binary_io_append(io, &events[2]) != 0 ||
        binary_io_append(io, &events[3]) != 0) {
        perror("binary_io_append ipc session");
        binary_io_close(io);
        return -1;
    }

    binary_io_close(io);
    io = NULL;

    if (hardware_mock_init() != 0) {
        perror("hardware_mock_init");
        return -1;
    }

    if (ipc_socket_init(&ipc) != 0) {
        perror("ipc_socket_init session");
        return -1;
    }

    ipc_socket_restore_session(ipc, "tmp/test_ipc_session.bin");

    if (sysfs_poll_sample(&sample) != 0) {
        perror("sysfs_poll_sample session");
        ipc_socket_shutdown(ipc, -1);
        return -1;
    }

    sysfs_poll_build_event(EV_TICK, &sample, &event);
    ipc_socket_update_cache(ipc, &event, &sample, 0);

    if (ipc_socket_test_build_json(ipc, json, sizeof(json)) != 0) {
        perror("ipc_socket_test_build_json session");
        ipc_socket_shutdown(ipc, -1);
        return -1;
    }

    if (sscanf(strstr(json, "\"session_mins\":"), "\"session_mins\":%u", &session_mins) != 1) {
        fprintf(stderr, "ipc session restore test failed: missing session_mins in %s", json);
        ipc_socket_shutdown(ipc, -1);
        return -1;
    }

    if (session_mins < 2U) {
        fprintf(stderr,
                "ipc session restore test failed: expected session_mins >= 2, got %u in %s",
                session_mins, json);
        ipc_socket_shutdown(ipc, -1);
        return -1;
    }

    ipc_socket_shutdown(ipc, -1);
    printf("ipc session restore test passed: session_mins=%u\n", session_mins);
    return 0;
}

static int test_ipc_session_scan_all_fallback(void)
{
    BinaryIo *io = NULL;
    IpcSocket *ipc = NULL;
    struct PowerLedgerEvent events[4];
    struct SysfsSample sample;
    struct PowerLedgerEvent event;
    char json[640];
    unsigned session_mins;
    uint32_t base_ts;

    if (mkdir("tmp", 0700) != 0 && errno != EEXIST) {
        return -1;
    }

    if (unlink("tmp/test_ipc_scan_all.bin") != 0 && errno != ENOENT) {
        return -1;
    }

    base_ts = test_monotonic_now_seconds();
    if (base_ts < 300U) {
        fprintf(stderr, "ipc scan-all fallback test skipped: monotonic clock too small\n");
        return 0;
    }
    base_ts -= 180U;

    memset(events, 0, sizeof(events));
    events[0].type = (uint8_t)EV_UNPLUG;
    events[0].timestamp = base_ts;
    events[1].type = (uint8_t)EV_TICK;
    events[1].timestamp = base_ts + 60U;
    events[2].type = (uint8_t)EV_TICK;
    events[2].timestamp = base_ts + 120U;
    events[3].type = (uint8_t)EV_UNPLUG;
    events[3].timestamp = base_ts + 180U;

    if (binary_io_open(&io, "tmp/test_ipc_scan_all.bin") != 0) {
        perror("binary_io_open scan-all fallback");
        return -1;
    }

    if (binary_io_append(io, &events[0]) != 0 ||
        binary_io_append(io, &events[1]) != 0 ||
        binary_io_append(io, &events[2]) != 0 ||
        binary_io_append(io, &events[3]) != 0) {
        perror("binary_io_append scan-all fallback");
        binary_io_close(io);
        return -1;
    }

    binary_io_close(io);
    io = NULL;

    if (hardware_mock_init() != 0) {
        perror("hardware_mock_init");
        return -1;
    }

    if (ipc_socket_init(&ipc) != 0) {
        perror("ipc_socket_init scan-all fallback");
        return -1;
    }

    ipc_socket_restore_session(ipc, "tmp/test_ipc_scan_all.bin");

    if (sysfs_poll_sample(&sample) != 0) {
        perror("sysfs_poll_sample scan-all fallback");
        ipc_socket_shutdown(ipc, -1);
        return -1;
    }

    sysfs_poll_build_event(EV_TICK, &sample, &event);
    ipc_socket_update_cache(ipc, &event, &sample, 0);

    if (ipc_socket_test_build_json(ipc, json, sizeof(json)) != 0) {
        perror("ipc_socket_test_build_json scan-all fallback");
        ipc_socket_shutdown(ipc, -1);
        return -1;
    }

    if (sscanf(strstr(json, "\"session_mins\":"), "\"session_mins\":%u", &session_mins) != 1) {
        fprintf(stderr, "ipc scan-all fallback test failed: missing session_mins in %s", json);
        ipc_socket_shutdown(ipc, -1);
        return -1;
    }

    if (session_mins < 2U) {
        fprintf(stderr,
                "ipc scan-all fallback test failed: expected session_mins >= 2, got %u in %s",
                session_mins, json);
        ipc_socket_shutdown(ipc, -1);
        return -1;
    }

    ipc_socket_shutdown(ipc, -1);
    printf("ipc scan-all fallback test passed: session_mins=%u\n", session_mins);
    return 0;
}

static int test_ipc_session_all_modes(void)
{
    BinaryIo *io = NULL;
    IpcSocket *ipc = NULL;
    struct PowerLedgerEvent events[6];
    struct SysfsSample sample;
    struct PowerLedgerEvent event;
    char json_session[640];
    char json_all[640];
    char ascii_session[384];
    char ascii_all[384];
    unsigned session_mins = 0U;
    unsigned all_mins = 0U;

    if (mkdir("tmp", 0700) != 0 && errno != EEXIST) {
        return -1;
    }

    if (unlink("tmp/test_ipc_all_modes.bin") != 0 && errno != ENOENT) {
        return -1;
    }

    memset(events, 0, sizeof(events));
    events[0].type = (uint8_t)EV_PLUG;
    events[0].timestamp = 1000U;
    events[1].type = (uint8_t)EV_UNPLUG;
    events[1].timestamp = 1060U;
    events[2].type = (uint8_t)EV_TICK;
    events[2].timestamp = 1120U;
    events[3].type = (uint8_t)EV_PLUG;
    events[3].timestamp = 1180U;
    events[4].type = (uint8_t)EV_TICK;
    events[4].timestamp = 1240U;
    events[5].type = (uint8_t)EV_TICK;
    events[5].timestamp = 1300U;

    if (binary_io_open(&io, "tmp/test_ipc_all_modes.bin") != 0) {
        perror("binary_io_open ipc all modes");
        return -1;
    }

    if (binary_io_append(io, &events[0]) != 0 ||
        binary_io_append(io, &events[1]) != 0 ||
        binary_io_append(io, &events[2]) != 0 ||
        binary_io_append(io, &events[3]) != 0 ||
        binary_io_append(io, &events[4]) != 0 ||
        binary_io_append(io, &events[5]) != 0) {
        perror("binary_io_append ipc all modes");
        binary_io_close(io);
        return -1;
    }

    binary_io_close(io);
    io = NULL;

    if (hardware_mock_init() != 0) {
        perror("hardware_mock_init");
        return -1;
    }

    if (ipc_socket_init(&ipc) != 0) {
        perror("ipc_socket_init all modes");
        return -1;
    }

    ipc_socket_restore_session(ipc, "tmp/test_ipc_all_modes.bin");

    if (sysfs_poll_sample(&sample) != 0) {
        perror("sysfs_poll_sample all modes");
        ipc_socket_shutdown(ipc, -1);
        return -1;
    }

    sysfs_poll_build_event(EV_TICK, &sample, &event);
    ipc_socket_update_cache(ipc, &event, &sample, 0);

    if (ipc_socket_test_build_json(ipc, json_session, sizeof(json_session)) != 0 ||
        ipc_socket_test_build_json_all(ipc, json_all, sizeof(json_all)) != 0 ||
        ipc_socket_test_build_ascii(ipc, ascii_session, sizeof(ascii_session)) != 0 ||
        ipc_socket_test_build_ascii_all(ipc, ascii_all, sizeof(ascii_all)) != 0) {
        perror("ipc all-modes build");
        ipc_socket_shutdown(ipc, -1);
        return -1;
    }

    if (sscanf(strstr(json_session, "\"session_mins\":"), "\"session_mins\":%u",
               &session_mins) != 1 ||
        sscanf(strstr(json_all, "\"session_mins\":"), "\"session_mins\":%u", &all_mins) !=
            1) {
        fprintf(stderr, "ipc all-modes test failed: missing session_mins\n");
        ipc_socket_shutdown(ipc, -1);
        return -1;
    }

    if (session_mins != 2U || all_mins != 3U) {
        fprintf(stderr,
                "ipc all-modes test failed: expected session=2 all=3, got %u/%u\n",
                session_mins, all_mins);
        ipc_socket_shutdown(ipc, -1);
        return -1;
    }

    if (strstr(ascii_session, "session_mins=2 ") == NULL &&
        strstr(ascii_session, "session_mins=2\n") == NULL) {
        fprintf(stderr, "ipc all-modes ASCII session failed: %s", ascii_session);
        ipc_socket_shutdown(ipc, -1);
        return -1;
    }

    if (strstr(ascii_all, "session_mins=3 ") == NULL &&
        strstr(ascii_all, "session_mins=3\n") == NULL) {
        fprintf(stderr, "ipc all-modes ASCII all failed: %s", ascii_all);
        ipc_socket_shutdown(ipc, -1);
        return -1;
    }

    ipc_socket_shutdown(ipc, -1);
    printf("ipc all-modes test passed: session_mins=%u all_mins=%u\n", session_mins,
           all_mins);
    return 0;
}

static int test_ipc_never_empty(void)
{
    IpcSocket *ipc = NULL;
    char json[640];
    char ascii[384];

    if (hardware_mock_init() != 0) {
        perror("hardware_mock_init");
        return -1;
    }

    if (ipc_socket_init(&ipc) != 0) {
        perror("ipc_socket_init never empty");
        return -1;
    }

    if (ipc_socket_test_build_json(ipc, json, sizeof(json)) != 0) {
        perror("ipc_socket_test_build_json never empty");
        ipc_socket_shutdown(ipc, -1);
        return -1;
    }

    if (json[0] == '\0' || strstr(json, "\"regime\":") == NULL ||
        strstr(json, "\"status\":") == NULL) {
        fprintf(stderr, "ipc never-empty JSON test failed: %s", json);
        ipc_socket_shutdown(ipc, -1);
        return -1;
    }

    if (ipc_socket_test_build_ascii(ipc, ascii, sizeof(ascii)) != 0) {
        perror("ipc_socket_test_build_ascii never empty");
        ipc_socket_shutdown(ipc, -1);
        return -1;
    }

    if (ascii[0] == '\0' || strstr(ascii, "regime=") == NULL ||
        strstr(ascii, "status=") == NULL) {
        fprintf(stderr, "ipc never-empty ASCII test failed: %s", ascii);
        ipc_socket_shutdown(ipc, -1);
        return -1;
    }

    ipc_socket_shutdown(ipc, -1);
    printf("ipc never-empty test passed\n");
    return 0;
}

static int test_ipc_battery_eta_json(void)
{
    IpcSocket *ipc = NULL;
    struct SysfsSample sample;
    struct PowerLedgerEvent event;
    char json[640];

    if (hardware_mock_init() != 0) {
        perror("hardware_mock_init");
        return -1;
    }

    if (hardware_mock_set_ac_online(0) != 0 ||
        hardware_mock_set_bat_status("Discharging") != 0 ||
        hardware_mock_set_bat_power(15000000L) != 0 ||
        hardware_mock_set_bat_energy(36000000L, 50000000L) != 0) {
        perror("hardware_mock ipc setup");
        return -1;
    }

    if (sysfs_poll_sample(&sample) != 0) {
        perror("sysfs_poll_sample");
        return -1;
    }

    sysfs_poll_build_event(EV_TICK, &sample, &event);

    if (ipc_socket_init(&ipc) != 0) {
        perror("ipc_socket_init");
        return -1;
    }

    ipc_socket_update_cache(ipc, &event, &sample, 1);
    if (ipc_socket_test_build_json(ipc, json, sizeof(json)) != 0) {
        perror("ipc_socket_test_build_json");
        ipc_socket_shutdown(ipc, -1);
        return -1;
    }

    if (strstr(json, "\"remaining\":\"2h 24m\"") == NULL) {
        fprintf(stderr, "ipc ETA JSON test failed: missing remaining field in %s", json);
        ipc_socket_shutdown(ipc, -1);
        return -1;
    }

    if (strstr(json, "},\"remaining\"") != NULL) {
        fprintf(stderr, "ipc ETA JSON test failed: malformed object in %s", json);
        ipc_socket_shutdown(ipc, -1);
        return -1;
    }

    if (strstr(json, "to_full") != NULL) {
        fprintf(stderr, "ipc ETA JSON test failed: unexpected to_full in %s", json);
        ipc_socket_shutdown(ipc, -1);
        return -1;
    }

    ipc_socket_shutdown(ipc, -1);
    printf("ipc ETA JSON test passed\n");
    return 0;
}

static int test_ipc_battery_eta_ascii(void)
{
    IpcSocket *ipc = NULL;
    struct SysfsSample sample;
    struct PowerLedgerEvent event;
    char line[384];

    if (hardware_mock_init() != 0) {
        perror("hardware_mock_init");
        return -1;
    }

    if (hardware_mock_set_ac_online(0) != 0 ||
        hardware_mock_set_bat_status("Discharging") != 0 ||
        hardware_mock_set_bat_power(15000000L) != 0 ||
        hardware_mock_set_bat_energy(36000000L, 50000000L) != 0) {
        perror("hardware_mock ipc ascii setup");
        return -1;
    }

    if (sysfs_poll_sample(&sample) != 0) {
        perror("sysfs_poll_sample");
        return -1;
    }

    sysfs_poll_build_event(EV_TICK, &sample, &event);

    if (ipc_socket_init(&ipc) != 0) {
        perror("ipc_socket_init");
        return -1;
    }

    ipc_socket_update_cache(ipc, &event, &sample, 1);
    if (ipc_socket_test_build_ascii(ipc, line, sizeof(line)) != 0) {
        perror("ipc_socket_test_build_ascii");
        ipc_socket_shutdown(ipc, -1);
        return -1;
    }

    if (strstr(line, " remaining=2h 24m") == NULL) {
        fprintf(stderr, "ipc ETA ASCII test failed: missing remaining field in %s", line);
        ipc_socket_shutdown(ipc, -1);
        return -1;
    }

    if (strstr(line, "session_mins=") == NULL) {
        fprintf(stderr, "ipc ETA ASCII test failed: missing session_mins in %s", line);
        ipc_socket_shutdown(ipc, -1);
        return -1;
    }

    ipc_socket_shutdown(ipc, -1);
    printf("ipc ETA ASCII test passed\n");
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

    if (test_duration_format() != 0) {
        rc = EXIT_FAILURE;
    }

    if (test_mock_sample() != 0) {
        rc = EXIT_FAILURE;
    }

    if (test_session_start_restore() != 0) {
        rc = EXIT_FAILURE;
    }

    if (test_session_active_seconds() != 0) {
        rc = EXIT_FAILURE;
    }

    if (test_session_wake_not_boundary() != 0) {
        rc = EXIT_FAILURE;
    }

    if (test_session_terminal_unplug_ignored() != 0) {
        rc = EXIT_FAILURE;
    }

    if (test_discharge_stops_at_plug() != 0) {
        rc = EXIT_FAILURE;
    }

    if (test_discharge_spurious_unplug_ignored() != 0) {
        rc = EXIT_FAILURE;
    }

    if (test_discharge_on_ac_zero() != 0) {
        rc = EXIT_FAILURE;
    }

    if (test_sysfs_battery_eta_discharge() != 0) {
        rc = EXIT_FAILURE;
    }

    if (test_sysfs_battery_eta_charge() != 0) {
        rc = EXIT_FAILURE;
    }

    if (test_sysfs_battery_eta_full() != 0) {
        rc = EXIT_FAILURE;
    }

    if (test_ipc_session_mins_restore() != 0) {
        rc = EXIT_FAILURE;
    }

    if (test_ipc_session_mins_zero_on_ac() != 0) {
        rc = EXIT_FAILURE;
    }

    if (test_ipc_session_scan_all_fallback() != 0) {
        rc = EXIT_FAILURE;
    }

    if (test_ipc_never_empty() != 0) {
        rc = EXIT_FAILURE;
    }

    if (test_ipc_session_all_modes() != 0) {
        rc = EXIT_FAILURE;
    }

    if (test_ipc_battery_eta_json() != 0) {
        rc = EXIT_FAILURE;
    }

    if (test_ipc_battery_eta_ascii() != 0) {
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
