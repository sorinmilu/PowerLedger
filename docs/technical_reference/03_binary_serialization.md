# Binary Serialization Reference

This document defines the on-disk format for Power Ledger event records stored in `/var/log/power_ledger.bin`. Each record is a fixed 16-byte packed structure with no headers, delimiters, or trailing text.

## Record Layout

| Offset | Size | Field           | Type     | Description |
|--------|------|-----------------|----------|-------------|
| 0      | 1    | `type`          | `uint8_t` | Event type (`ledger_event_t`) |
| 1      | 1    | `battery_level` | `uint8_t` | Battery capacity percentage (0–100) |
| 2      | 2    | `fan_speed`     | `uint16_t` | Fan speed in RPM (0–65535) |
| 4      | 4    | `power_drain`   | `int32_t` | Signed power in microwatts (negative = discharging, positive = charging) |
| 8      | 4    | `timestamp`     | `uint32_t` | Monotonic uptime seconds (`CLOCK_MONOTONIC_RAW`) |
| 12     | 1    | `power_regime`  | `uint8_t` | Performance regime (`power_regime_t`) |
| 13     | 3    | `reserved`      | `uint8_t[3]` | Must be written as `0x00` |

Total record size: **16 bytes**.

## Event Types (`ledger_event_t`)

| Value | Symbol     | Meaning |
|-------|------------|---------|
| 1     | `EV_PLUG`  | AC adapter connected |
| 2     | `EV_UNPLUG`| AC adapter disconnected |
| 3     | `EV_SLEEP` | System entered suspend/sleep |
| 4     | `EV_WAKE`  | System resumed from suspend |
| 5     | `EV_TICK`  | Periodic heartbeat snapshot |

## Power Regimes (`power_regime_t`)

| Value | Symbol            | Typical source strings |
|-------|-------------------|------------------------|
| 1     | `REG_PERFORMANCE` | `performance`, `default` |
| 2     | `REG_BALANCED`    | `balanced`, `balance_performance` |
| 3     | `REG_POWER_SAVE`  | `power`, `balance_power`, `power-save` |
| 4     | `REG_QUIET`       | `quiet`, `low-power` |

## Structure Definition

```c
struct __attribute__((__packed__)) PowerLedgerEvent {
    uint8_t type;
    uint8_t battery_level;
    uint16_t fan_speed;
    int32_t power_drain;
    uint32_t timestamp;
    uint8_t power_regime;
    uint8_t reserved[3];
};
```

Compile-time enforcement: `_Static_assert(sizeof(struct PowerLedgerEvent) == 16, ...)`.

## File I/O Policy

- **Open flags:** `O_WRONLY | O_CREAT | O_APPEND`
- **Permissions:** `0600` (root read/write only)
- **Default path:** `/var/log/power_ledger.bin`
- **Write contract:** Each append must write exactly 16 bytes. Partial writes are treated as structural failure.
- **Durability:** After every successful write, `fdatasync()` flushes data to stable storage to survive abrupt power loss.
- **Format:** Pure binary array of sequential records. No JSON, no newline-terminated text, no file header.

## IPC and CLI Consumers

The ledger file is read by:

- `bat-time` — full-file or discharge-window Riemann integration (see `05_riemann_math.md`)
- Daemon IPC — `session_mins` only, on each `0x4A` / `0x4B` / `0x51` / `0x52` query (see `04_ipc_json_protocol.md`)

Live sysfs snapshots in IPC responses are not re-read from the ledger; only integrated runtime minutes are derived from on-disk records.

## Validation

The layout test (`make test`) writes 100 sequential dummy records to a local test file and verifies via `stat()` that the file size is exactly **1600 bytes** (100 × 16).
