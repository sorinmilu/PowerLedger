# IPC Protocol Reference

Power Ledger exposes a local UNIX domain stream socket for low-latency telemetry queries. Each transaction is a single request byte in, one response line out, then the connection closes.

## Endpoint

| Property | Value |
|----------|-------|
| Transport | `AF_UNIX` `SOCK_STREAM` |
| Path | `/run/power_ledger.sock` |
| Permissions | `0666` after bind |
| Listener | Non-blocking; accepted clients are serviced synchronously on `accept()` |

## Request Protocol

1. Client connects to the socket path.
2. Client writes **one byte** request header (blocking write is fine).
3. Daemon waits for the byte (poll-backed read), refreshes live sysfs into the in-memory cache, optionally scans the ledger for `session_mins`, serializes the response, writes one line terminated by `\n`, and **closes** the connection.

### Request Headers

| Byte | ASCII | Mode | `session_mins` source |
|------|-------|------|------------------------|
| `0x4A` | `J` | JSON session | Discharge window (`bat-time` default) |
| `0x4B` | `K` | JSON all | Full ledger (`bat-time --all`) |
| `0x51` | `Q` | ASCII session | Discharge window |
| `0x52` | `R` | ASCII all | Full ledger |

Any other header byte closes the connection with no payload.

### Session vs all

Live fields (`status`, `pct`, `watts`, `regime`, `rpm`, `remaining`, `to_full`) are identical for all four headers. Only `session_mins` differs:

| Mode | Integration rules | On AC now |
|------|-------------------|-----------|
| Session (`0x4A`, `0x51`) | See [discharge window](05_riemann_math.md#discharge-window-default) | `0` |
| All (`0x4B`, `0x52`) | Entire ledger, record 0 → tail | Not forced to `0` |

Use **session** for status bars that should reset when the adapter is plugged in. Use **all** for cumulative runtime matching `bat-time --all`.

## Data Sources per Response

| Field group | Source on each IPC query |
|-------------|--------------------------|
| `status`, `pct`, `watts`, `regime`, `rpm`, `ts` | Fresh `sysfs_poll_sample()` → in-memory cache |
| `remaining`, `to_full` | Derived from sysfs ETA fields at sample time |
| `session_mins` | **Cached** in daemon memory (no ledger read on the IPC hot path) |

### `session_mins` cache refresh

The daemon maintains cached totals as follows:

- **Startup / plug / unplug / sleep events:** full ledger integration (`session_refresh_caches_from_ledger`)
- **60-second `EV_TICK`:** incremental advance by the tick interval (no full-file scan)
- **IPC poll while on AC:** discharge-session cache is zeroed from sysfs without reading the ledger
- **IPC poll on AC→battery transition:** one full rescan to re-anchor the discharge window

IPC queries never scan the ledger file. `session_mins` advances on each heartbeat tick.

The 16-byte ledger record format is not streamed over IPC.

## JSON Response Schema (`0x4A`, `0x4B`)

`0x4A` and `0x4B` emit the same JSON shape; only `session_mins` may differ.

Example lines:

```json
{"status":"discharging","pct":74,"watts":-4.85,"regime":"power-save","rpm":1800,"session_mins":142,"remaining":"5h 12m"}
{"status":"charging","pct":74,"watts":12.50,"regime":"balanced","rpm":1800,"session_mins":0,"to_full":"1h 08m"}
```

| Key | Type | Unit / meaning |
|-----|------|----------------|
| `status` | string | `discharging`, `charging`, or `idle` (derived from signed `power_drain`) |
| `pct` | integer | Battery capacity percentage (0–100) |
| `watts` | float | Instantaneous power in watts (`power_drain` µW ÷ 1,000,000) |
| `regime` | string | `performance`, `balanced`, `power-save`, or `quiet` |
| `rpm` | integer | Fan speed |
| `session_mins` | integer | Active runtime in whole minutes (see [session vs all](#session-vs-all)) |
| `remaining` | string | Human-readable time-to-empty estimate (on battery, discharging only) |
| `to_full` | string | Human-readable time-to-full-charge estimate (on AC, charging only) |

`remaining` and `to_full` are omitted when not applicable (for example AC connected at 100%, or idle on wall power). Values are formatted as `42m` or `5h 12m` (zero-padded minutes when hours are present). Internally the daemon stores integer minutes; formatting happens at IPC serialization time.

## ASCII Response Schema (`0x51`, `0x52`)

`0x51` and `0x52` emit the same key/value layout; only `session_mins` may differ.

Example lines:

```text
status=discharging pct=74 watts=-4.85 regime=power-save rpm=1800 session_mins=142 ts=123456 remaining=5h 12m
status=charging pct=74 watts=12.50 regime=balanced rpm=1800 session_mins=0 ts=123457 to_full=1h 08m
```

Fields mirror the JSON payload. `session_mins` follows the same session/all rules as JSON. `ts` is monotonic uptime seconds from the cached record. `remaining` and `to_full` use the same human-readable duration strings as JSON and are omitted when not applicable.

## Reliability Guarantees

- **Never empty:** Valid `0x4A` / `0x4B` / `0x51` / `0x52` requests always receive a parseable line. If sysfs sampling or payload construction fails, the daemon emits a safe fallback (`status=idle`, `regime=balanced`, `session_mins=0`, and so on).
- **Synchronous service:** Each `accept()` completes read → build → write → `close()` in the listener thread. Clients are not left on a secondary epoll path.
- **Request-byte wait:** The daemon polls up to ~5 s for the single request byte before abandoning the connection.

## Client Examples

### socat

```bash
# JSON — discharge session (resets on AC)
printf '\x4A' | socat - UNIX-CONNECT:/run/power_ledger.sock

# JSON — full ledger (bat-time --all)
printf '\x4B' | socat - UNIX-CONNECT:/run/power_ledger.sock

# ASCII — discharge session
printf '\x51' | socat - UNIX-CONNECT:/run/power_ledger.sock

# ASCII — full ledger
printf '\x52' | socat - UNIX-CONNECT:/run/power_ledger.sock
```

### netcat

```bash
printf '\x4A' | nc -U /run/power_ledger.sock
printf '\x4B' | nc -U /run/power_ledger.sock
printf '\x51' | nc -U /run/power_ledger.sock
printf '\x52' | nc -U /run/power_ledger.sock
```

Some `nc` builds require `-N` to close after EOF:

```bash
printf '\x4B' | nc -U -N /run/power_ledger.sock
```

## Lifecycle

- The daemon unlinks `/run/power_ledger.sock` on startup (before bind) and again during graceful shutdown after `SIGINT` / `SIGTERM`.
- Each accepted client is serviced with fixed stack buffers (`char[640]` for JSON, `char[384]` for ASCII) and no per-request heap allocation.

## Related Documentation

- Discharge-window math and ledger integration rules: [05_riemann_math.md](05_riemann_math.md)
- Sysfs fields (`regime`, ETA): [01_hardware_sysfs.md](01_hardware_sysfs.md)
- End-user quick start: [../user_manual.md](../user_manual.md)
