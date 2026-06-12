#define _POSIX_C_SOURCE 200809L

#include "ledger_format.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "binary_io.h"
#include "duration_format.h"
#include "sysfs_poll.h"

#define LEDGER_DEFAULT_PATH "/var/log/power_ledger.bin"
#define READ_CHUNK_RECORDS  64

struct RiemannResult {
    double active_seconds;
    double energy_wh;
};

static int is_session_boundary(uint8_t type)
{
    return type == (uint8_t)EV_UNPLUG;
}

static double segment_delta_seconds(const struct PowerLedgerEvent *left,
                                    const struct PowerLedgerEvent *right)
{
    uint32_t delta;

    if (left == NULL || right == NULL) {
        return 0.0;
    }

    if (left->type == (uint8_t)EV_SLEEP || right->type == (uint8_t)EV_WAKE) {
        return 0.0;
    }

    if (left->type == (uint8_t)EV_PLUG) {
        return 0.0;
    }

    if (right->timestamp < left->timestamp) {
        return 0.0;
    }

    delta = right->timestamp - left->timestamp;
    return (double)delta;
}

static double segment_energy_wh(const struct PowerLedgerEvent *left, double delta_seconds)
{
    double watts;
    double hours;

    if (left == NULL || delta_seconds <= 0.0) {
        return 0.0;
    }

    if (left->power_drain >= 0) {
        return 0.0;
    }

    watts = (double)(-left->power_drain) / 1000000.0;
    hours = delta_seconds / 3600.0;
    return watts * hours;
}

static void integrate_segment(const struct PowerLedgerEvent *left,
                              const struct PowerLedgerEvent *right,
                              struct RiemannResult *out)
{
    double delta;

    if (left == NULL || right == NULL || out == NULL) {
        return;
    }

    delta = segment_delta_seconds(left, right);
    if (delta <= 0.0) {
        return;
    }

    out->active_seconds += delta;
    out->energy_wh += segment_energy_wh(left, delta);
}

static int integrate_events(const struct PowerLedgerEvent *events, size_t count,
                            struct RiemannResult *out)
{
    size_t i;

    if (events == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(out, 0, sizeof(*out));

    if (count < 2U) {
        return 0;
    }

    for (i = 0; i + 1U < count; i++) {
        integrate_segment(&events[i], &events[i + 1U], out);
    }

    return 0;
}

static void format_duration(double active_seconds, char *buf, size_t buflen)
{
    unsigned long total_minutes;
    unsigned long hours;
    unsigned long minutes;

    if (buf == NULL || buflen == 0U) {
        return;
    }

    total_minutes = (unsigned long)(active_seconds / 60.0);
    hours = total_minutes / 60UL;
    minutes = total_minutes % 60UL;

    (void)snprintf(buf, buflen, "%luh %lum", hours, minutes);
}

static int read_records_chunk(int fd, off_t base_offset, size_t record_count,
                              struct PowerLedgerEvent *out, size_t out_cap,
                              size_t *out_read)
{
    ssize_t nread;
    size_t want;

    if (out == NULL || out_read == NULL || out_cap == 0U) {
        errno = EINVAL;
        return -1;
    }

    want = record_count;
    if (want > out_cap) {
        want = out_cap;
    }

    if (lseek(fd, base_offset, SEEK_SET) < 0) {
        return -1;
    }

    nread = read(fd, out, want * sizeof(struct PowerLedgerEvent));
    if (nread < 0) {
        return -1;
    }
    if (nread % (ssize_t)sizeof(struct PowerLedgerEvent) != 0) {
        errno = EIO;
        return -1;
    }

    *out_read = (size_t)nread / sizeof(struct PowerLedgerEvent);
    return 0;
}

static int scan_session_start_backward(int fd, off_t file_size, int scan_all,
                                       size_t *out_start_index)
{
    struct PowerLedgerEvent chunk[READ_CHUNK_RECORDS];
    off_t offset;
    size_t records_in_file;
    size_t global_index;
    size_t nread;
    size_t i;

    if (out_start_index == NULL) {
        errno = EINVAL;
        return -1;
    }

    records_in_file = (size_t)file_size / sizeof(struct PowerLedgerEvent);
    if (records_in_file == 0U) {
        *out_start_index = 0U;
        return 0;
    }

    if (scan_all) {
        *out_start_index = 0U;
        return 0;
    }

    global_index = records_in_file;
    offset = file_size;

    while (offset > 0) {
        off_t chunk_off;
        size_t chunk_records;

        if (offset >= (off_t)(READ_CHUNK_RECORDS * sizeof(struct PowerLedgerEvent))) {
            chunk_off = offset - (off_t)(READ_CHUNK_RECORDS * sizeof(struct PowerLedgerEvent));
            chunk_records = READ_CHUNK_RECORDS;
        } else {
            chunk_off = 0;
            chunk_records = (size_t)offset / sizeof(struct PowerLedgerEvent);
        }

        if (read_records_chunk(fd, chunk_off, chunk_records, chunk, READ_CHUNK_RECORDS,
                               &nread) != 0) {
            return -1;
        }

        for (i = nread; i-- > 0U;) {
            global_index--;
            if (is_session_boundary(chunk[i].type) &&
                global_index + 1U < records_in_file) {
                *out_start_index = global_index;
                return 0;
            }
        }

        offset = chunk_off;
    }

    *out_start_index = 0U;
    return 0;
}

static int analyze_ledger(const char *path, int scan_all, struct RiemannResult *out)
{
    struct stat st;
    int fd = -1;
    struct PowerLedgerEvent *window = NULL;
    size_t window_cap = READ_CHUNK_RECORDS;
    int have_prev = 0;
    struct PowerLedgerEvent prev_event;
    size_t start_index = 0U;
    size_t total_records;
    size_t i;
    off_t offset;
    size_t nread;

    if (path == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(out, 0, sizeof(*out));

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    if (fstat(fd, &st) != 0) {
        (void)close(fd);
        return -1;
    }

    if (st.st_size <= 0 ||
        st.st_size % (off_t)sizeof(struct PowerLedgerEvent) != 0) {
        errno = EIO;
        (void)close(fd);
        return -1;
    }

    if (scan_session_start_backward(fd, st.st_size, scan_all, &start_index) != 0) {
        (void)close(fd);
        return -1;
    }

    total_records = (size_t)st.st_size / sizeof(struct PowerLedgerEvent);
    if (start_index >= total_records) {
        (void)close(fd);
        return 0;
    }

    window = calloc(window_cap, sizeof(*window));
    if (window == NULL) {
        (void)close(fd);
        return -1;
    }

    offset = (off_t)start_index * (off_t)sizeof(struct PowerLedgerEvent);

    while (offset < st.st_size) {
        size_t remaining;
        size_t want;

        remaining = (size_t)((st.st_size - offset) / (off_t)sizeof(struct PowerLedgerEvent));
        want = remaining;
        if (want > window_cap) {
            want = window_cap;
        }

        if (read_records_chunk(fd, offset, want, window, window_cap, &nread) != 0) {
            free(window);
            (void)close(fd);
            return -1;
        }

        if (nread == 0U) {
            break;
        }

        if (have_prev) {
            integrate_segment(&prev_event, &window[0], out);
        }

        for (i = 0; i + 1U < nread; i++) {
            integrate_segment(&window[i], &window[i + 1U], out);
        }

        prev_event = window[nread - 1U];
        have_prev = 1;

        offset += (off_t)nread * (off_t)sizeof(struct PowerLedgerEvent);
    }

    free(window);
    (void)close(fd);
    return 0;
}

static int run_frozen_clock_self_test(void)
{
    struct PowerLedgerEvent timeline[5];
    struct RiemannResult result;
    const int32_t five_w_uw = -5000000;
    const uint32_t two_hours = 2U * 3600U;
    const uint32_t day_gap = 24U * 3600U;
    const uint32_t one_hour = 3600U;

    memset(timeline, 0, sizeof(timeline));

    timeline[0].type = (uint8_t)EV_UNPLUG;
    timeline[0].power_drain = five_w_uw;
    timeline[0].timestamp = 0U;

    timeline[1].type = (uint8_t)EV_TICK;
    timeline[1].power_drain = five_w_uw;
    timeline[1].timestamp = two_hours;

    timeline[2].type = (uint8_t)EV_SLEEP;
    timeline[2].power_drain = five_w_uw;
    timeline[2].timestamp = two_hours;

    timeline[3].type = (uint8_t)EV_WAKE;
    timeline[3].power_drain = five_w_uw;
    timeline[3].timestamp = two_hours + day_gap;

    timeline[4].type = (uint8_t)EV_TICK;
    timeline[4].power_drain = five_w_uw;
    timeline[4].timestamp = two_hours + day_gap + one_hour;

    if (integrate_events(timeline, 5U, &result) != 0) {
        return -1;
    }

    if (fabs(result.active_seconds - 10800.0) > 0.5) {
        fprintf(stderr,
                "frozen clock failed: expected 10800 active seconds, got %.2f\n",
                result.active_seconds);
        return -1;
    }

    if (fabs(result.energy_wh - 15.0) > 0.01) {
        fprintf(stderr,
                "frozen clock failed: expected 15.0 Wh, got %.4f\n",
                result.energy_wh);
        return -1;
    }

    printf("frozen clock self-test passed\n");
    return 0;
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

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [-f ledger_path] [--all] [--self-test]\n", prog);
}

int main(int argc, char **argv)
{
    const char *ledger_path = LEDGER_DEFAULT_PATH;
    struct RiemannResult result;
    struct BinaryIoDischargeResult discharge;
    struct PowerLedgerEvent last_event;
    char duration[64];
    double avg_watts;
    int on_ac_now = 0;
    static const struct option long_opts[] = {
        {"all", no_argument, NULL, 'a'},
        {"self-test", no_argument, NULL, 't'},
        {NULL, 0, NULL, 0},
    };
    int scan_all = 0;
    int self_test = 0;
    int opt;

    while ((opt = getopt_long(argc, argv, "f:aht", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'f':
            ledger_path = optarg;
            break;
        case 'a':
            scan_all = 1;
            break;
        case 't':
            self_test = 1;
            break;
        case 'h':
            usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (self_test) {
        return run_frozen_clock_self_test() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (scan_all) {
        if (analyze_ledger(ledger_path, 1, &result) != 0) {
            perror("bat-time");
            return EXIT_FAILURE;
        }
    } else {
        if (binary_io_read_last_event(ledger_path, &last_event) == 0 &&
            last_event.power_drain >= 0) {
            on_ac_now = 1;
        }

        if (binary_io_analyze_discharge(ledger_path, on_ac_now, &discharge) != 0) {
            perror("bat-time");
            return EXIT_FAILURE;
        }

        result.active_seconds = (double)discharge.active_seconds;
        result.energy_wh = discharge.energy_wh;
    }

    format_duration(result.active_seconds, duration, sizeof(duration));

    if (result.active_seconds > 0.0) {
        avg_watts = result.energy_wh / (result.active_seconds / 3600.0);
    } else {
        avg_watts = 0.0;
    }

    if (binary_io_read_last_event(ledger_path, &last_event) != 0) {
        memset(&last_event, 0, sizeof(last_event));
        last_event.power_regime = (uint8_t)REG_BALANCED;
    }

    printf("Power Regime           : %s\n", regime_to_string(last_event.power_regime));
    printf("Active Session Runtime : %s\n", duration);
    printf("Net Energy Consumed    : %.2f Wh\n", result.energy_wh);
    printf("Average Session Burn   : %.2fW\n", avg_watts);

    {
        struct SysfsSample sample;
        char eta[32];

        if (sysfs_poll_sample(&sample) == 0) {
            if (sample.remaining_mins >= 0) {
                duration_format_mins(sample.remaining_mins, eta, sizeof(eta));
                printf("Remaining              : %s\n", eta);
            }
            if (sample.to_full_mins >= 0) {
                duration_format_mins(sample.to_full_mins, eta, sizeof(eta));
                printf("To Full                : %s\n", eta);
            }
        }
    }

    return EXIT_SUCCESS;
}
