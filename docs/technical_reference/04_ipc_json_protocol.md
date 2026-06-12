# IPC JSON Protocol Reference

Power Ledger exposes a local UNIX domain stream socket for low-latency telemetry queries. The daemon serves cached in-memory metrics only; it does not read `/var/log/power_ledger.bin` during IPC transactions.

## Endpoint

| Property | Value |
|----------|-------|
| Transport | `AF_UNIX` `SOCK_STREAM` |
| Path | `/run/power_ledger.sock` |
| Permissions | `0666` after bind |
| Blocking mode | Non-blocking listener and accepted clients |

## Request Protocol

1. Client connects to the socket path.
2. Client writes **one byte** request header.
3. Daemon reads the header, serializes the latest cached `PowerLedgerEvent`, writes the response, and **closes** the connection.

### Request Headers

| Byte | Symbol | Response |
|------|--------|----------|
| `0x51` | ASCII query | Plain-text key/value line terminated by `\n` |
| `0x4A` | JSON query | Single-line minified JSON terminated by `\n` |

Any other header byte results in an immediate connection close with no payload.

## JSON Response Schema (`0x4A`)

Example line:

```json
{"status":"discharging","pct":74,"watts":-4.85,"regime":"power-save","rpm":1800,"session_mins":142}
```

| Key | Type | Unit / meaning |
|-----|------|----------------|
| `status` | string | `discharging`, `charging`, or `idle` (derived from signed `power_drain`) |
| `pct` | integer | Battery capacity percentage (0–100) |
| `watts` | float | Instantaneous power in watts (`power_drain` µW ÷ 1,000,000) |
| `regime` | string | `performance`, `balanced`, `power-save`, or `quiet` |
| `rpm` | integer | Fan speed |
| `session_mins` | integer | Minutes since the most recent `EV_WAKE` or `EV_UNPLUG` boundary |

## ASCII Response Schema (`0x51`)

Example line:

```text
status=discharging pct=74 watts=-4.85 regime=power-save rpm=1800 ts=123456
```

Fields mirror the JSON payload. `ts` is the monotonic uptime seconds from the cached record.

## Client Examples

JSON widget poll:

```bash
printf '\x4A' | socat - UNIX-CONNECT:/run/power_ledger.sock
```

ASCII CLI poll:

```bash
printf '\x51' | socat - UNIX-CONNECT:/run/power_ledger.sock
```

## Lifecycle

- The daemon unlinks `/run/power_ledger.sock` on startup (before bind) and again during graceful shutdown after `SIGINT` / `SIGTERM`.
- Each accepted client is serviced synchronously from the epoll thread using fixed stack buffers (`char[512]` for JSON) with no per-request heap allocation.
