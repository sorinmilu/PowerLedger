#ifndef SYSFS_POLL_H
#define SYSFS_POLL_H

#include "ledger_format.h"

#define SYSFS_ETA_NA (-1)

struct SysfsSample {
    int32_t power_drain;
    uint16_t fan_speed;
    uint8_t battery_level;
    uint8_t power_regime;
    uint8_t ac_online;
    int32_t remaining_mins;
    int32_t to_full_mins;
    uint32_t timestamp;
};

const char *sysfs_poll_resolve_path(const char *sys_path);
int sysfs_poll_open_ac_fd(void);
int sysfs_poll_read_ac_online(int ac_fd);
int sysfs_poll_ac_debounce_accept(ledger_event_t ev);
void sysfs_poll_ac_debounce_reset(void);
int sysfs_poll_sample(struct SysfsSample *out);
void sysfs_poll_build_event(ledger_event_t type, const struct SysfsSample *sample,
                            struct PowerLedgerEvent *out);

#endif /* SYSFS_POLL_H */
