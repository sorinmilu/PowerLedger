#define _POSIX_C_SOURCE 200809L

#include "binary_io.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
