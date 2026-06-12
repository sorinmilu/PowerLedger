# D-Bus Lifecycle Reference

Power Ledger listens on the system D-Bus for `systemd-logind` suspend and resume milestones. These signals bracket sleep dead-zones so downstream Riemann integration (`bat-time`, Phase 4) can exclude frozen clock intervals from active power accounting.

## Match Rule

The daemon subscribes with a single additive match string:

```
type='signal',sender='org.freedesktop.login1',interface='org.freedesktop.login1.Manager',member='PrepareForSleep',path='/org/freedesktop/login1'
```

`dbus_bus_add_match()` is called once at startup. `dbus_bus_remove_match()` runs during `dbus_handler_shutdown()`.

## PrepareForSleep Payload

| Argument | D-Bus type | Meaning |
|----------|------------|---------|
| `sleeping` | `boolean` | `true` = kernel is about to suspend; `false` = resume completed |

| `sleeping` value | Ledger event | When recorded |
|------------------|--------------|---------------|
| `true` | `EV_SLEEP` (3) | Immediately before the process scheduler freezes |
| `false` | `EV_WAKE` (4) | As soon as userspace resumes after suspend |

Each lifecycle record is a full 16-byte `PowerLedgerEvent` built from a fresh `sysfs_poll_sample()` snapshot (battery power, level, fan RPM, power regime, monotonic timestamp).

## Epoll Integration

1. `dbus_bus_get(DBUS_BUS_SYSTEM)` opens the system bus.
2. `dbus_connection_get_unix_fd()` yields the raw socket descriptor.
3. `fcntl(fd, F_SETFL, flags | O_NONBLOCK)` keeps the wire socket non-blocking.
4. `main.c` registers that descriptor on the master `epoll` instance with `EPOLLIN`.
5. On wake, `dbus_handler_dispatch()` calls `dbus_connection_read_write_dispatch(conn, 0)` in a tight loop until no further data is available—no blocking D-Bus calls in the event path.

A connection-level message filter handles `PrepareForSleep` and appends to `/var/log/power_ledger.bin` via `binary_io_append()` (which `fdatasync()`s each record).

## Signal Flow

```
systemd-logind                libdbus socket              epoll loop              binary log
      |                              |                         |                        |
      |-- PrepareForSleep(true) ---->|                         |                        |
      |                              |-- EPOLLIN ------------->|                        |
      |                              |                         |-- dispatch ----------->|
      |                              |                         |   sysfs sample         |
      |                              |                         |   EV_SLEEP + fsync --->|
      |  [system suspends]           |                         |                        |
      |                              |                         |                        |
      |-- PrepareForSleep(false) --->|                         |                        |
      |                              |-- EPOLLIN ------------->|                        |
      |                              |                         |-- dispatch ----------->|
      |                              |                         |   sysfs sample         |
      |                              |                         |   EV_WAKE + fsync ---->|
```

## EV_SLEEP Critical Path

`PrepareForSleep(true)` arrives seconds before the kernel suspends. The filter callback must:

- Avoid heap allocations beyond the existing ledger write path.
- Sample sysfs and append synchronously so the pre-sleep snapshot reaches disk before freeze.

`EV_WAKE` follows the same append path after resume; fresh hardware registers reflect post-wake state.

## Failure Modes

| Condition | Daemon behavior |
|-----------|-----------------|
| System bus unavailable at startup | `dbus_handler_init()` logs to `stderr` and returns `-1`; daemon continues without sleep tracking |
| Match rule rejected | Init cleans up connection and returns `-1` |
| Sysfs sample fails during signal | Error logged; signal still consumed to avoid re-delivery loops |

## Shutdown

`dbus_handler_shutdown()` removes the epoll registration, drops the match rule, removes the message filter, and `dbus_connection_unref()` the connection.

## TEST_MODE

The unit test harness (`make test`) compiles sysfs and binary I/O modules only; it does not link `dbus_handler.c` or start the daemon. Tests remain valid in environments without a running system bus.
