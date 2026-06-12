# Riemann Integration Reference

The `bat-time` client (`src/client/bat_time.c`) estimates active battery energy drain from the append-only binary ledger at `/var/log/power_ledger.bin`. The algorithm is a **piecewise left-hand Riemann sum** over consecutive 16-byte records, with explicit sleep dead-zone pruning.

## Inputs

Each record is a packed `PowerLedgerEvent` (see `03_binary_serialization.md`). Integration uses:

- `timestamp` — monotonic seconds (`CLOCK_MONOTONIC_RAW`)
- `power_drain` — signed microwatts (negative while discharging)
- `type` — event discriminator, especially `EV_SLEEP` (3) and `EV_WAKE` (4)

## Session Boundary Scan

By default, `bat-time` scans **backward** from the file tail in fixed chunks (64 records) until it finds the most recent `EV_WAKE` or `EV_UNPLUG`. Integration runs forward from that index to the final record.

Pass `--all` to integrate the entire file without backward boundary detection.

## Interval Mathematics

For each adjacent pair \((i, i+1)\):

1. **Time delta**

   \[
   \Delta t = t_{i+1} - t_i
   \]

2. **Sleep dead-zone override**

   If `type_i == EV_SLEEP` **or** `type_{i+1} == EV_WAKE`, force \(\Delta t = 0\).

3. **Energy segment (left-hand sample)**

   When `power_drain_i < 0`:

   \[
   \text{Wh}_\text{segment} = \frac{|\text{power\_drain}_i|}{1{,}000{,}000} \times \frac{\Delta t}{3600}
   \]

4. **Accumulation**

   - `active_seconds += Δt` (only when \(\Delta t > 0\) after sleep pruning)
   - `energy_wh += Wh_segment`

5. **Average burn rate**

   \[
   \text{avg\_watts} = \frac{\text{energy\_wh}}{\text{active\_seconds} / 3600}
   \]

## Pseudocode

```text
function integrate(records[0..n-1]):
    active_seconds = 0
    energy_wh = 0
    for i in 0 .. n-2:
        dt = records[i+1].timestamp - records[i].timestamp
        if records[i].type == EV_SLEEP or records[i+1].type == EV_WAKE:
            dt = 0
        if dt <= 0:
            continue
        active_seconds += dt
        if records[i].power_drain < 0:
            watts = abs(records[i].power_drain) / 1e6
            energy_wh += watts * (dt / 3600.0)
    return (active_seconds, energy_wh)
```

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

The client streams ledger data with `lseek` + `read` in 64-record windows. It does not mmap or malloc the full history. Only one chunk buffer and the previous record bridge are retained while scanning forward.
