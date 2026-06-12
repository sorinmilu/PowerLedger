#define _POSIX_C_SOURCE 200809L

#include "binary_io.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SESSION_SCAN_CHUNK 64

struct BinaryIo {
    int fd;
};

int binary_io_open(BinaryIo **out, const char *path)
{
    const char *target;
    BinaryIo *io;

    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }

    target = (path != NULL) ? path : BINARY_IO_DEFAULT_PATH;
    io = calloc(1, sizeof(*io));
    if (io == NULL) {
        return -1;
    }

    io->fd = open(target, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (io->fd < 0) {
        free(io);
        return -1;
    }

    *out = io;
    return 0;
}

static int is_session_boundary(uint8_t type)
{
    return type == (uint8_t)EV_UNPLUG;
}

static int timestamp_usable(uint32_t ts, uint32_t now_ts)
{
    if (ts == 0U) {
        return 0;
    }
    if (now_ts == 0U) {
        return 1;
    }
    return ts <= now_ts;
}

int binary_io_find_session_start_ts(const char *path, uint32_t now_ts, uint32_t *out_ts)
{
    const char *target;
    struct stat st;
    struct PowerLedgerEvent chunk[SESSION_SCAN_CHUNK];
    struct PowerLedgerEvent first;
    int fd;
    off_t offset;
    ssize_t nread;
    size_t records_in_file;
    size_t i;

    if (out_ts == NULL) {
        errno = EINVAL;
        return -1;
    }

    *out_ts = 0U;
    target = (path != NULL) ? path : BINARY_IO_DEFAULT_PATH;

    fd = open(target, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            return 0;
        }
        return -1;
    }

    if (fstat(fd, &st) != 0) {
        (void)close(fd);
        return -1;
    }

    if (st.st_size <= 0) {
        (void)close(fd);
        return 0;
    }

    if (st.st_size % (off_t)sizeof(struct PowerLedgerEvent) != 0) {
        errno = EIO;
        (void)close(fd);
        return -1;
    }

    records_in_file = (size_t)st.st_size / sizeof(struct PowerLedgerEvent);
    (void)records_in_file;
    offset = st.st_size;

    while (offset > 0) {
        off_t chunk_off;
        size_t chunk_records;
        size_t nrec;

        if (offset >= (off_t)(SESSION_SCAN_CHUNK * sizeof(struct PowerLedgerEvent))) {
            chunk_off = offset - (off_t)(SESSION_SCAN_CHUNK * sizeof(struct PowerLedgerEvent));
            chunk_records = SESSION_SCAN_CHUNK;
        } else {
            chunk_off = 0;
            chunk_records = (size_t)offset / sizeof(struct PowerLedgerEvent);
        }

        if (lseek(fd, chunk_off, SEEK_SET) < 0) {
            (void)close(fd);
            return -1;
        }

        nread = read(fd, chunk, chunk_records * sizeof(struct PowerLedgerEvent));
        if (nread < 0) {
            (void)close(fd);
            return -1;
        }
        if ((size_t)nread % sizeof(struct PowerLedgerEvent) != 0U) {
            errno = EIO;
            (void)close(fd);
            return -1;
        }

        nrec = (size_t)nread / sizeof(struct PowerLedgerEvent);
        for (i = nrec; i-- > 0U;) {
            if (is_session_boundary(chunk[i].type) &&
                timestamp_usable(chunk[i].timestamp, now_ts)) {
                *out_ts = chunk[i].timestamp;
                (void)close(fd);
                return 0;
            }
        }

        offset = chunk_off;
    }

    if (lseek(fd, 0, SEEK_SET) < 0) {
        (void)close(fd);
        return -1;
    }

    nread = read(fd, &first, sizeof(first));
    (void)close(fd);

    if (nread == (ssize_t)sizeof(first) &&
        timestamp_usable(first.timestamp, now_ts)) {
        *out_ts = first.timestamp;
    }

    return 0;
}

static uint32_t segment_active_seconds(const struct PowerLedgerEvent *left,
                                       const struct PowerLedgerEvent *right)
{
    if (left == NULL || right == NULL) {
        return 0U;
    }

    if (left->type == (uint8_t)EV_SLEEP || right->type == (uint8_t)EV_WAKE) {
        return 0U;
    }

    if (left->type == (uint8_t)EV_PLUG) {
        return 0U;
    }

    if (right->timestamp < left->timestamp) {
        return 0U;
    }

    return right->timestamp - left->timestamp;
}

static double segment_energy_wh(const struct PowerLedgerEvent *left, uint32_t delta_seconds)
{
    double watts;

    if (left == NULL || delta_seconds == 0U || left->power_drain >= 0) {
        return 0.0;
    }

    watts = (double)(-left->power_drain) / 1000000.0;
    return watts * ((double)delta_seconds / 3600.0);
}

static void accumulate_segment(const struct PowerLedgerEvent *left,
                               const struct PowerLedgerEvent *right,
                               uint32_t *active_seconds, double *energy_wh)
{
    uint32_t delta;

    if (left == NULL || right == NULL || active_seconds == NULL) {
        return;
    }

    delta = segment_active_seconds(left, right);
    if (delta == 0U) {
        return;
    }

    *active_seconds += delta;
    if (energy_wh != NULL) {
        *energy_wh += segment_energy_wh(left, delta);
    }
}

static size_t find_session_end_index(int fd, off_t file_size, size_t start_index)
{
    struct PowerLedgerEvent ev;
    size_t total_records;
    size_t i;

    total_records = (size_t)file_size / sizeof(struct PowerLedgerEvent);
    if (start_index >= total_records) {
        return start_index;
    }

    for (i = start_index; i < total_records; i++) {
        if (lseek(fd, (off_t)i * (off_t)sizeof(ev), SEEK_SET) < 0) {
            return total_records;
        }
        if (read(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
            return total_records;
        }
        if (ev.type == (uint8_t)EV_PLUG) {
            return i + 1U;
        }
    }

    return total_records;
}

static int read_records_at(int fd, off_t offset, size_t record_count,
                           struct PowerLedgerEvent *out, size_t out_cap,
                           size_t *out_read);

static int integrate_range(int fd, off_t file_size, size_t start_index,
                           size_t end_exclusive, uint32_t *active_seconds,
                           double *energy_wh, struct PowerLedgerEvent *out_last_event,
                           int *out_last_valid)
{
    struct PowerLedgerEvent window[SESSION_SCAN_CHUNK];
    struct PowerLedgerEvent prev_event;
    off_t limit;
    off_t offset;
    size_t nread;
    size_t i;
    int have_prev = 0;

    if (active_seconds == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (start_index >= end_exclusive) {
        return 0;
    }

    limit = (off_t)end_exclusive * (off_t)sizeof(struct PowerLedgerEvent);
    if (limit > file_size) {
        limit = file_size;
    }

    offset = (off_t)start_index * (off_t)sizeof(struct PowerLedgerEvent);

    while (offset < limit) {
        size_t remaining;
        size_t want;

        remaining = (size_t)((limit - offset) / (off_t)sizeof(struct PowerLedgerEvent));
        want = remaining;
        if (want > SESSION_SCAN_CHUNK) {
            want = SESSION_SCAN_CHUNK;
        }

        if (read_records_at(fd, offset, want, window, SESSION_SCAN_CHUNK, &nread) != 0) {
            return -1;
        }

        if (nread == 0U) {
            break;
        }

        if (have_prev) {
            accumulate_segment(&prev_event, &window[0], active_seconds, energy_wh);
        }

        for (i = 0; i + 1U < nread; i++) {
            accumulate_segment(&window[i], &window[i + 1U], active_seconds, energy_wh);
        }

        prev_event = window[nread - 1U];
        have_prev = 1;

        offset += (off_t)nread * (off_t)sizeof(struct PowerLedgerEvent);
    }

    if (out_last_event != NULL && have_prev) {
        *out_last_event = prev_event;
    }
    if (out_last_valid != NULL) {
        *out_last_valid = have_prev;
    }

    return 0;
}

static int scan_session_start_index(int fd, off_t file_size, int scan_all,
                                    size_t *out_start_index)
{
    struct PowerLedgerEvent chunk[SESSION_SCAN_CHUNK];
    off_t offset;
    ssize_t nread;
    size_t global_index;
    size_t records_in_file;
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
        size_t nrec;

        if (offset >= (off_t)(SESSION_SCAN_CHUNK * sizeof(struct PowerLedgerEvent))) {
            chunk_off = offset - (off_t)(SESSION_SCAN_CHUNK * sizeof(struct PowerLedgerEvent));
            chunk_records = SESSION_SCAN_CHUNK;
        } else {
            chunk_off = 0;
            chunk_records = (size_t)offset / sizeof(struct PowerLedgerEvent);
        }

        if (lseek(fd, chunk_off, SEEK_SET) < 0) {
            return -1;
        }

        nread = read(fd, chunk, chunk_records * sizeof(struct PowerLedgerEvent));
        if (nread < 0) {
            return -1;
        }
        if ((size_t)nread % sizeof(struct PowerLedgerEvent) != 0U) {
            errno = EIO;
            return -1;
        }

        nrec = (size_t)nread / sizeof(struct PowerLedgerEvent);
        for (i = nrec; i-- > 0U;) {
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

static int has_prior_plug(int fd, size_t unplug_index)
{
    struct PowerLedgerEvent ev;
    size_t i;

    for (i = 0U; i < unplug_index; i++) {
        if (lseek(fd, (off_t)i * (off_t)sizeof(ev), SEEK_SET) < 0) {
            return 0;
        }
        if (read(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
            return 0;
        }
        if (ev.type == (uint8_t)EV_PLUG) {
            return 1;
        }
    }

    return 0;
}

static int scan_discharge_start_index(int fd, off_t file_size, size_t *out_start_index)
{
    struct PowerLedgerEvent chunk[SESSION_SCAN_CHUNK];
    off_t offset;
    ssize_t nread;
    size_t global_index;
    size_t records_in_file;
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

    global_index = records_in_file;
    offset = file_size;

    while (offset > 0) {
        off_t chunk_off;
        size_t chunk_records;
        size_t nrec;

        if (offset >= (off_t)(SESSION_SCAN_CHUNK * sizeof(struct PowerLedgerEvent))) {
            chunk_off = offset - (off_t)(SESSION_SCAN_CHUNK * sizeof(struct PowerLedgerEvent));
            chunk_records = SESSION_SCAN_CHUNK;
        } else {
            chunk_off = 0;
            chunk_records = (size_t)offset / sizeof(struct PowerLedgerEvent);
        }

        if (lseek(fd, chunk_off, SEEK_SET) < 0) {
            return -1;
        }

        nread = read(fd, chunk, chunk_records * sizeof(struct PowerLedgerEvent));
        if (nread < 0) {
            return -1;
        }
        if ((size_t)nread % sizeof(struct PowerLedgerEvent) != 0U) {
            errno = EIO;
            return -1;
        }

        nrec = (size_t)nread / sizeof(struct PowerLedgerEvent);
        for (i = nrec; i-- > 0U;) {
            global_index--;
            if (is_session_boundary(chunk[i].type) &&
                global_index + 1U < records_in_file &&
                has_prior_plug(fd, global_index)) {
                *out_start_index = global_index;
                return 0;
            }
        }

        offset = chunk_off;
    }

    *out_start_index = 0U;
    return 0;
}

static int read_records_at(int fd, off_t offset, size_t record_count,
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

    if (lseek(fd, offset, SEEK_SET) < 0) {
        return -1;
    }

    nread = read(fd, out, want * sizeof(struct PowerLedgerEvent));
    if (nread < 0) {
        return -1;
    }
    if ((size_t)nread % sizeof(struct PowerLedgerEvent) != 0U) {
        errno = EIO;
        return -1;
    }

    *out_read = (size_t)nread / sizeof(struct PowerLedgerEvent);
    return 0;
}

int binary_io_compute_session_active(const char *path, int scan_all,
                                     uint32_t *out_seconds,
                                     struct PowerLedgerEvent *out_last_event,
                                     int *out_last_valid)
{
    const char *target;
    struct stat st;
    struct PowerLedgerEvent window[SESSION_SCAN_CHUNK];
    struct PowerLedgerEvent prev_event;
    int fd;
    int have_prev = 0;
    off_t offset;
    size_t start_index;
    size_t total_records;
    size_t nread;
    size_t i;
    uint32_t active_seconds = 0U;

    if (out_seconds == NULL) {
        errno = EINVAL;
        return -1;
    }

    *out_seconds = 0U;
    if (out_last_valid != NULL) {
        *out_last_valid = 0;
    }

    target = (path != NULL) ? path : BINARY_IO_DEFAULT_PATH;

    fd = open(target, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            return 0;
        }
        return -1;
    }

    if (fstat(fd, &st) != 0) {
        (void)close(fd);
        return -1;
    }

    if (st.st_size <= 0) {
        (void)close(fd);
        return 0;
    }

    if (st.st_size % (off_t)sizeof(struct PowerLedgerEvent) != 0) {
        errno = EIO;
        (void)close(fd);
        return -1;
    }

    if (scan_session_start_index(fd, st.st_size, scan_all, &start_index) != 0) {
        (void)close(fd);
        return -1;
    }

    total_records = (size_t)st.st_size / sizeof(struct PowerLedgerEvent);
    if (start_index >= total_records) {
        (void)close(fd);
        return 0;
    }

    offset = (off_t)start_index * (off_t)sizeof(struct PowerLedgerEvent);

    while (offset < st.st_size) {
        size_t remaining;
        size_t want;

        remaining = (size_t)((st.st_size - offset) / (off_t)sizeof(struct PowerLedgerEvent));
        want = remaining;
        if (want > SESSION_SCAN_CHUNK) {
            want = SESSION_SCAN_CHUNK;
        }

        if (read_records_at(fd, offset, want, window, SESSION_SCAN_CHUNK, &nread) != 0) {
            (void)close(fd);
            return -1;
        }

        if (nread == 0U) {
            break;
        }

        if (have_prev) {
            active_seconds += segment_active_seconds(&prev_event, &window[0]);
        }

        for (i = 0; i + 1U < nread; i++) {
            active_seconds += segment_active_seconds(&window[i], &window[i + 1U]);
        }

        prev_event = window[nread - 1U];
        have_prev = 1;

        offset += (off_t)nread * (off_t)sizeof(struct PowerLedgerEvent);
    }

    (void)close(fd);

    *out_seconds = active_seconds;
    if (out_last_event != NULL && have_prev) {
        *out_last_event = prev_event;
    }
    if (out_last_valid != NULL) {
        *out_last_valid = have_prev;
    }

    return 0;
}

int binary_io_read_last_event(const char *path, struct PowerLedgerEvent *out_event)
{
    const char *target;
    struct stat st;
    struct PowerLedgerEvent last;
    int fd;
    ssize_t nread;

    if (out_event == NULL) {
        errno = EINVAL;
        return -1;
    }

    target = (path != NULL) ? path : BINARY_IO_DEFAULT_PATH;

    fd = open(target, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    if (fstat(fd, &st) != 0) {
        (void)close(fd);
        return -1;
    }

    if (st.st_size < (off_t)sizeof(struct PowerLedgerEvent) ||
        st.st_size % (off_t)sizeof(struct PowerLedgerEvent) != 0) {
        errno = EIO;
        (void)close(fd);
        return -1;
    }

    if (lseek(fd, st.st_size - (off_t)sizeof(last), SEEK_SET) < 0) {
        (void)close(fd);
        return -1;
    }

    nread = read(fd, &last, sizeof(last));
    (void)close(fd);

    if (nread != (ssize_t)sizeof(last)) {
        errno = EIO;
        return -1;
    }

    *out_event = last;
    return 0;
}

int binary_io_analyze_discharge(const char *path, int on_ac_now,
                                struct BinaryIoDischargeResult *out_result)
{
    const char *target;
    struct stat st;
    int fd;
    size_t start_index;
    size_t end_exclusive;
    uint32_t active_seconds = 0U;
    double energy_wh = 0.0;

    if (out_result == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(out_result, 0, sizeof(*out_result));
    if (on_ac_now) {
        return 0;
    }

    target = (path != NULL) ? path : BINARY_IO_DEFAULT_PATH;

    fd = open(target, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            return 0;
        }
        return -1;
    }

    if (fstat(fd, &st) != 0) {
        (void)close(fd);
        return -1;
    }

    if (st.st_size <= 0) {
        (void)close(fd);
        return 0;
    }

    if (st.st_size % (off_t)sizeof(struct PowerLedgerEvent) != 0) {
        errno = EIO;
        (void)close(fd);
        return -1;
    }

    if (scan_discharge_start_index(fd, st.st_size, &start_index) != 0) {
        (void)close(fd);
        return -1;
    }

    end_exclusive = find_session_end_index(fd, st.st_size, start_index);

    if (integrate_range(fd, st.st_size, start_index, end_exclusive, &active_seconds,
                        &energy_wh, NULL, NULL) != 0) {
        (void)close(fd);
        return -1;
    }

    (void)close(fd);

    out_result->active_seconds = active_seconds;
    out_result->energy_wh = energy_wh;
    return 0;
}

int binary_io_append(BinaryIo *io, const struct PowerLedgerEvent *event)
{
    struct PowerLedgerEvent record;
    ssize_t written;

    if (io == NULL || event == NULL || io->fd < 0) {
        errno = EINVAL;
        return -1;
    }

    memcpy(&record, event, sizeof(record));
    memset(record.reserved, 0, sizeof(record.reserved));

    written = write(io->fd, &record, sizeof(record));
    if (written < 0) {
        return -1;
    }
    if ((size_t)written != sizeof(record)) {
        errno = EIO;
        return -1;
    }

    if (fdatasync(io->fd) != 0) {
        return -1;
    }

    return 0;
}

void binary_io_close(BinaryIo *io)
{
    if (io == NULL) {
        return;
    }

    if (io->fd >= 0) {
        (void)close(io->fd);
        io->fd = -1;
    }

    free(io);
}
