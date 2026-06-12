# Riemann Integration Reference

The `bat-time` client (`src/client/bat_time.c`) and the daemon IPC layer (`src/daemon/binary_io.c`) estimate active battery time and energy from the append-only binary ledger at `/var/log/power_ledger.bin`. The algorithm is a **piecewise left-hand Riemann sum** over consecutive 16-byte records, with explicit sleep dead-zone pruning.

## Inputs

Each record is a packed `PowerLedgerEvent` (see `03_binary_serialization.md`). Integration uses:

- `timestamp` — monotonic seconds (`CLOCK_MONOTONIC_RAW`)
- `power_drain` — signed microwatts (negative while discharging)
- `type` — event discriminator (`EV_PLUG`, `EV_UNPLUG`, `EV_SLEEP`, `EV_WAKE`, `EV_TICK`)

## Discharge Window (default)

By default, `bat-time` and IPC session modes (`0x4A`, `0x51`) integrate the **current battery discharge window** via `binary_io_analyze_discharge()`:

- **On AC now** (`ac_online` from sysfs, or last record `power_drain >= 0` for `bat-time`): return `0` active seconds.
- **Start index:** most recent `EV_UNPLUG` that is not the final record **and** is preceded by an earlier `EV_PLUG` (real AC disconnect). Spurious `EV_UNPLUG` records logged without a prior plug (for example at daemon restart while already on battery) are ignored; integration then starts at record 0.
- **End index:** first `EV_PLUG` after the start index (exclusive), or end of file while still on battery.
- **Terminal `EV_UNPLUG`:** an `EV_UNPLUG` as the last record with no following tick is ignored when scanning backward.

Sleep gaps are excluded via the segment rule below. Segments whose left edge is `EV_PLUG` contribute \(\Delta t = 0\).

Pass `bat-time --all` or use IPC `0x4B` / `0x52` to integrate the **entire ledger** without discharge-window boundaries (`binary_io_compute_session_active(..., scan_all=1)`). On AC, full-ledger mode still reports cumulative history (it does not force zero).

## Interval Mathematics

For each adjacent pair \((i, i+1)\):

1. **Time delta**

   \[
   \Delta t = t_{i+1} - t_i
   \]

2. **Sleep dead-zone override**

   If `type_i == EV_SLEEP` **or** `type_{i+1} == EV_WAKE`, force \(\Delta t = 0\).

3. **AC plug left-edge override**

   If `type_i == EV_PLUG`, force \(\Delta t = 0\) (post-plug segments do not extend a discharge window).

4. **Energy segment (left-hand sample)**

   When `power_drain_i < 0`:

   \[
   \text{Wh}_\text{segment} = \frac{|\text{power\_drain}_i|}{1{,}000{,}000} \times \frac{\Delta t}{3600}
   \]

5. **Accumulation**

   - `active_seconds += Δt` (only when \(\Delta t > 0\) after pruning)
   - `energy_wh += Wh_segment`

6. **Average burn rate**

   \[
   \text{avg\_watts} = \frac{\text{energy\_wh}}{\text{active\_seconds} / 3600}
   \]

## Pseudocode

```text
function segment_delta(left, right):
    if left.type == EV_SLEEP or right.type == EV_WAKE:
        return 0
    if left.type == EV_PLUG:
        return 0
    return max(0, right.timestamp - left.timestamp)

function integrate_range(records[start..end)):
    active_seconds = 0
    energy_wh = 0
    for i in start .. end-2:
        dt = segment_delta(records[i], records[i+1])
        if dt <= 0:
            continue
        active_seconds += dt
        if records[i].power_drain < 0:
            watts = abs(records[i].power_drain) / 1e6
            energy_wh += watts * (dt / 3600.0)
    return (active_seconds, energy_wh)
```

## bat-time CLI

| Invocation | Integration |
|------------|-------------|
| `bat-time` (default) | Discharge window (`binary_io_analyze_discharge`) |
| `bat-time --all` | Full ledger (`analyze_ledger(..., scan_all=1)`) |

Example output:

```text
Power Regime           : power-save
Active Session Runtime : 2h 15m
Net Energy Consumed    : 8.42 Wh
Average Session Burn   : 3.74W
```

`Power Regime` is read from the last ledger record's `power_regime` field.

## IPC session_mins

IPC exposes the same integration modes through request-byte selection (see `04_ipc_json_protocol.md`):

| Request | `session_mins` |
|---------|----------------|
| `0x4A`, `0x51` | Discharge window (matches default `bat-time`) |
| `0x4B`, `0x52` | Full ledger (matches `bat-time --all`) |

`session_mins = active_seconds / 60` (integer division). Live sysfs fields are refreshed on every IPC query. Session totals are **cached** in the daemon and recomputed on timer ticks, AC events, and AC-state changes — not during each socket poll.

## Frozen Clock Verification

`bat-time --self-test` validates the sleep-gap rule with a synthetic timeline:

| Segment | Duration | Power | Notes |
|---------|----------|-------|-------|
| Active drain | 2 h | 5.0 W | Normal interval |
| Sleep gap | 24 h | — | `EV_SLEEP` → `EV_WAKE` forces \(\Delta t = 0\) |
| Active drain | 1 h | 5.0 W | Post-resume interval |

Expected totals:

- Active runtime: **3 hours** (10,800 seconds), not 27 hours
- Net energy: **15.0 Wh**

## Memory Model

The client and daemon stream ledger data with `lseek` + `read` in 64-record windows. Neither path mmap's or malloc's the full history. Only one chunk buffer and the previous record bridge are retained while scanning forward.
