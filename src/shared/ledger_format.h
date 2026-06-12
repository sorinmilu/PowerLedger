#ifndef LEDGER_FORMAT_H
#define LEDGER_FORMAT_H

#include <stdint.h>

typedef enum {
    EV_PLUG   = 1, /* AC power adapter connected */
    EV_UNPLUG = 2, /* AC power adapter disconnected */
    EV_SLEEP  = 3, /* System entered suspend/sleep state */
    EV_WAKE   = 4, /* System woke up from suspend state */
    EV_TICK   = 5  /* Heartbeat interval snapshot entry */
} ledger_event_t;

typedef enum {
    REG_PERFORMANCE = 1,
    REG_BALANCED    = 2,
    REG_POWER_SAVE  = 3,
    REG_QUIET       = 4
} power_regime_t;

struct __attribute__((__packed__)) PowerLedgerEvent {
    uint8_t type;          /* 1 Byte: maps to ledger_event_t enum */
    uint8_t battery_level; /* 1 Byte: capacity percentage (0 to 100) */
    uint16_t fan_speed;    /* 2 Bytes: fan speed in RPM (0 to 65535) */
    int32_t power_drain;   /* 4 Bytes: signed micro-watts (Negative=Draining, Positive=Charging) */
    uint32_t timestamp;    /* 4 Bytes: monotonic kernel uptime seconds (CLOCK_MONOTONIC_RAW) */
    uint8_t power_regime;  /* 1 Byte: maps to power_regime_t enum */
    uint8_t reserved[3];   /* 3 Bytes: explicit alignment padding bytes (Must write 0x00) */
};

_Static_assert(sizeof(struct PowerLedgerEvent) == 16,
               "PowerLedgerEvent must be exactly 16 bytes");

#endif /* LEDGER_FORMAT_H */
