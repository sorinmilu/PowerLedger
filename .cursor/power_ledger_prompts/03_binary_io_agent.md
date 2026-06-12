# 03_binary_io_[agent.md](http://agent.md) (Binary Storage & Serialization Specification)

This prompt targets the strict definition, bitwise compilation safety, and atomic writing mechanisms of the binary storage timeline layer for `power_ledger_d`. All tracking metrics must be saved as sequential uncompressed records inside `/var/log/power_ledger.bin` adhering down to the exact byte to the structure specified below.

## 1. System Components to Implement

You are responsible for writing and validating the following data module files:

- `src/shared/ledger_format.h`: The absolute source of truth for the memory data structures, containing packed structures and strict integer definitions.
- `src/daemon/binary_io.c`: The core serialization utility functions that handle safe file appending, descriptor states, and data writes to disk.

## 2. Technical Requirements

### 2.1 Direct Data Layout (Memory Alignment)

- Declare the data structure explicitly utilizing the GNU C compiler `__attribute__((__packed__))` flag to eliminate automatic compiler word alignment padding.
- The file structure must map exactly to this 16-byte memory footprint:

C

```
#ifndef LEDGER_FORMAT_H
#define LEDGER_FORMAT_H

#include <stdint.h>

typedef enum {
    EV_PLUG      = 1,  /* AC power adapter connected */
    EV_UNPLUG    = 2,  /* AC power adapter disconnected */
    EV_SLEEP     = 3,  /* System entered suspend/sleep state */
    EV_WAKE      = 4,  /* System woke up from suspend state */
    EV_TICK      = 5   /* Heartbeat interval snapshot entry */
} ledger_event_t;

typedef enum {
    REG_PERFORMANCE = 1,
    REG_BALANCED    = 2,
    REG_POWER_SAVE  = 3,
    REG_QUIET       = 4
} power_regime_t;

struct __attribute__((__packed__)) PowerLedgerEvent {
    uint8_t type;           /* 1 Byte: maps to ledger_event_t enum */
    uint8_t battery_level;  /* 1 Byte: capacity percentage (0 to 100) */
    uint16_t fan_speed;     /* 2 Bytes: fan speed in RPM (0 to 65535) */
    int32_t power_drain;    /* 4 Bytes: signed micro-watts (Negative=Draining, Positive=Charging) */
    uint32_t timestamp;     /* 4 Bytes: monotonic kernel uptime seconds (CLOCK_MONOTONIC_RAW) */
    uint8_t power_regime;   /* 1 Byte: maps to power_regime_t enum */
    uint8_t reserved[3];    /* 3 Bytes: explicit alignment padding bytes (Must write 0x00) */
};

#endif /* LEDGER_FORMAT_H */

```

### 2.2 Atomic Append-Only I/O Operations

- When initializing the storage subsystem, open `/var/log/power_ledger.bin` using low-level Unix file descriptors with flags `O_WRONLY | O_CREAT | O_APPEND` and permissions `0600` (root exclusive read/write).
- Every file operation writing a `PowerLedgerEvent` instance must be verified to write exactly 16 bytes. If the returned byte count from `write()` does not equal `sizeof(struct PowerLedgerEvent)`, the write must be aborted, rolled back if possible, and a structural system failure triggered.
- **Crash Resilience:** Immediately after invoking a valid write call, trigger an explicit `fdatasync(fd)` or `fsync(fd)` system flush. This bypasses the operating system OS page caches and forces the physical SSD controller to commit the 16 bytes directly to physical storage instantly, securing data integrity against power failure.

## 3. Verification & Guardrails

- **The Byte Layer Test Matrix:** You must provide a self-contained unit test suite within a mockup framework that writes exactly 100 sequential dummy records into a local file block. The validation routine must verify using `stat()` that the final output size matches exactly 1,600 bytes. If it matches 1,601 or 1,599 bytes, fail the build pipeline.
- **No Dynamic Strings:** No text headers, no timestamp formatting, and no trailing string newlines are permitted inside the storage file. The database is a pure, dense binary array of these sequential structures.

## 4. Execution Command for Cursor

When this prompt is linked to Cursor Composer along with the primary project blueprint, execute Phase 1 by implementing the underlying header format and completing the file appending engine with strict block validation.

**Commit Target:** `git commit -m "feat(ledger): implement packed structural specifications and verify file append layout"`