# Hardware Sysfs Reference

Power Ledger reads kernel telemetry through read-only sysfs nodes. The daemon never polls these paths in a tight loop; samples are taken only when the 60-second `timerfd` heartbeat fires (`EV_TICK`) or when the AC adapter online file reports an edge-triggered change (`EV_PLUG` / `EV_UNPLUG`).

## Path Map

| Metric | Primary sysfs path | Fallback | Parsed type |
|--------|-------------------|----------|-------------|
| AC online | `/sys/class/power_supply/AC0/online` | `/sys/class/power_supply/ADP1/online` | `1` = connected, `0` = disconnected |
| Battery power | `/sys/class/power_supply/BAT0/power_now` | — | microwatts (`int32_t`) |
| Battery status | `/sys/class/power_supply/BAT0/status` | — | `Discharging` negates `power_now` |
| Battery level | `/sys/class/power_supply/BAT0/capacity` | — | percent 0–100 |
| Fan RPM | `/sys/class/hwmon/hwmonN/fan1_input` | scan `hwmon*/name` | unsigned RPM |
| Power regime | `/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference` | `/sys/firmware/acpi/platform_profile` | maps to `power_regime_t` |

## Fan Index Discovery

The daemon scans `hwmon0` through `hwmon31` under `/sys/class/hwmon/`. For each directory it reads `name` and accepts the first match of:

- `asus`
- `coretemp`

The cached directory's `fan1_input` file supplies the RPM value written into the 16-byte ledger record.

## Power Regime Mapping

| Source string | `power_regime_t` |
|---------------|------------------|
| `performance`, `default` | `REG_PERFORMANCE` (1) |
| `balanced`, `balance_performance` | `REG_BALANCED` (2) |
| `power`, `balance_power` | `REG_POWER_SAVE` (3) |
| `quiet`, `low-power` | `REG_QUIET` (4) |

If neither cpufreq nor platform profile nodes are readable, the daemon records `REG_BALANCED`.

## AC Edge Monitoring

1. Open the AC online node read-only with `O_NONBLOCK`.
2. Register the descriptor on the epoll instance with `EPOLLIN | EPOLLET`.
3. On wake, drain the descriptor and `lseek()` back to offset 0 before reading the ASCII `0` or `1` value.

## Debounce Rules

Mechanical adapter contacts can generate repeated identical edge notifications within a few milliseconds. Before appending to `/var/log/power_ledger.bin`, the daemon applies a **3000 ms** debounce window keyed on `CLOCK_MONOTONIC_RAW` microseconds:

- Track the timestamp and event type (`EV_PLUG` or `EV_UNPLUG`) of the last accepted AC transition.
- If another **identical** event type arrives fewer than 3000 ms later, the record is dropped as contact-line jitter.
- Heartbeat `EV_TICK` records are not subject to this filter.

## TEST_MODE Sandbox

When compiled with `-DTEST_MODE` (`make test`), absolute `/sys/...` paths are rewritten to `./mock_sys/...` by stripping the `/sys/` prefix. The harness in `tests/hardware_mock.c` seeds a minimal tree:

```
mock_sys/class/power_supply/BAT0/{power_now,status,capacity}
mock_sys/class/power_supply/AC0/online
mock_sys/class/hwmon/hwmon0/{name,fan1_input}
mock_sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference
```

`tests/runner.c` mutates `AC0/online`, exercises the debounce gate, and verifies that rapid identical events within 50 ms produce exactly one 16-byte ledger record. The harness removes `mock_sys/` and temporary ledger files on completion.

## Resource Discipline

- Sampling helpers open sysfs files, read a single value, and `close()` the descriptor immediately.
- The AC online descriptor remains open for the lifetime of the daemon because it is registered with epoll for edge notifications.
- No `sleep()`, `usleep()`, or busy-wait loops are used in the daemon event path.
