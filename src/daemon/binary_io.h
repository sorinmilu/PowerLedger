#ifndef BINARY_IO_H
#define BINARY_IO_H

#include "ledger_format.h"

#define BINARY_IO_DEFAULT_PATH "/var/log/power_ledger.bin"

typedef struct BinaryIo BinaryIo;

struct BinaryIoDischargeResult {
    uint32_t active_seconds;
    double energy_wh;
};

int binary_io_open(BinaryIo **out, const char *path);
int binary_io_append(BinaryIo *io, const struct PowerLedgerEvent *event);
int binary_io_find_session_start_ts(const char *path, uint32_t now_ts, uint32_t *out_ts);
int binary_io_compute_session_active(const char *path, int scan_all,
                                     uint32_t *out_seconds,
                                     struct PowerLedgerEvent *out_last_event,
                                     int *out_last_valid);
int binary_io_read_last_event(const char *path, struct PowerLedgerEvent *out_event);
int binary_io_analyze_discharge(const char *path, int on_ac_now,
                                struct BinaryIoDischargeResult *out_result);
void binary_io_close(BinaryIo *io);

#endif /* BINARY_IO_H */
