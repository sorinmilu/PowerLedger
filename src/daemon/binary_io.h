#ifndef BINARY_IO_H
#define BINARY_IO_H

#include "ledger_format.h"

#define BINARY_IO_DEFAULT_PATH "/var/log/power_ledger.bin"

typedef struct BinaryIo BinaryIo;

int binary_io_open(BinaryIo **out, const char *path);
int binary_io_append(BinaryIo *io, const struct PowerLedgerEvent *event);
void binary_io_close(BinaryIo *io);

#endif /* BINARY_IO_H */
