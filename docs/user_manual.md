# Power Ledger User Manual

Power Ledger is a Linux daemon that records battery telemetry, AC plug events, and suspend/resume milestones to an append-only binary log. A local UNIX socket exposes live metrics for status bars and widgets; the `bat-time` CLI integrates the log with sleep dead-zones removed.

## Installation (Fedora)

Install build dependencies with `dnf5`:

```bash
sudo dnf5 install gcc make pkgconf-pkg-config dbus-devel
```

Clone the repository, build, and install binaries:

```bash
git clone <repository-url> power_ledger
cd power_ledger
make clean && make all
sudo install -m 0755 bin/power_ledger_d /usr/local/bin/
sudo install -m 0755 bin/bat-time /usr/local/bin/
```

Optional: run the in-tree regression suite before installing:

```bash
make test
make valgrind-test   # skipped automatically if valgrind is not installed
```

## Systemd Service

Create `/etc/systemd/system/power_ledger.service`:

```ini
[Unit]
Description=Power Ledger battery and power accounting daemon
Documentation=file:///usr/local/share/doc/power_ledger/user_manual.md
After=dbus.service
Wants=dbus.service

[Service]
Type=simple
ExecStart=/usr/local/bin/power_ledger_d -f /var/log/power_ledger.bin
Restart=on-failure
RestartSec=5

# Daemon binds /run/power_ledger.sock (0666) for local IPC clients.
RuntimeDirectoryMode=0755

[Install]
WantedBy=multi-user.target
```

Enable and start the service:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now power_ledger.service
sudo systemctl status power_ledger.service
```

Verify the IPC socket is listening:

```bash
ls -l /run/power_ledger.sock
```

## Binary Log and CLI (`bat-time`)

| Path | Purpose |
|------|---------|
| `/var/log/power_ledger.bin` | Append-only 16-byte event ledger |
| `/run/power_ledger.sock` | Live telemetry UNIX stream socket |
| `/usr/local/bin/bat-time` | Riemann energy integration CLI |

### Default: discharge session

Integrates the current battery discharge window (stops at `EV_PLUG`, shows `0` on AC):

```bash
bat-time
```

### Full ledger

Integrates the entire log without discharge-window boundaries:

```bash
bat-time --all
```

### Custom ledger path

```bash
bat-time -f /var/log/power_ledger.bin
```

Example output:

```text
Power Regime           : power-save
Active Session Runtime : 2h 15m
Net Energy Consumed    : 8.42 Wh
Average Session Burn   : 3.74W
Remaining              : 5h 12m
```

On AC while charging, `To Full` may appear instead of `Remaining`. Lines are omitted when the estimate is not available (same rules as IPC `remaining` / `to_full`).

## IPC Polling (Widgets and Dashboards)

Connect to `/run/power_ledger.sock`, write **one request byte**, read one response line, then disconnect.

### Quick reference

| Byte | Format | Use when |
|------|--------|----------|
| `0x4A` | JSON | Widgets; `session_mins` = discharge session (resets on AC) |
| `0x4B` | JSON | Widgets; `session_mins` = full ledger (`bat-time --all`) |
| `0x51` | ASCII | Shell scripts; discharge session |
| `0x52` | ASCII | Shell scripts; full ledger |

Live fields (`status`, `pct`, `watts`, `regime`, `rpm`, `remaining`, `to_full`) are refreshed on every poll. `session_mins` is served from a daemon cache (updated on each 60s tick, AC plug/unplug, and startup); it may lag by up to one minute between ticks. Only the session vs all mode selection changes which cached total is returned.

### JSON (`0x4A` session, `0x4B` all)

```bash
printf '\x4A' | socat - UNIX-CONNECT:/run/power_ledger.sock
printf '\x4B' | socat - UNIX-CONNECT:/run/power_ledger.sock
```

Example responses:

```json
{"status":"discharging","pct":74,"watts":-4.85,"regime":"power-save","rpm":1800,"session_mins":61,"remaining":"5h 12m"}
{"status":"charging","pct":74,"watts":12.50,"regime":"balanced","rpm":1800,"session_mins":0,"to_full":"1h 08m"}
```

With `0x4B`, the same machine on battery might instead show `"session_mins":61` (full history) while `0x4A` shows the discharge-window value.

| Key | Type | Meaning |
|-----|------|---------|
| `status` | string | `discharging`, `charging`, or `idle` |
| `pct` | integer | Battery capacity percentage (0–100) |
| `watts` | float | Instantaneous power in watts |
| `regime` | string | `performance`, `balanced`, `power-save`, or `quiet` |
| `rpm` | integer | Fan speed |
| `session_mins` | integer | See [quick reference](#quick-reference) |
| `remaining` | string | Time left on battery; omitted when N/A |
| `to_full` | string | Time until full charge; omitted when N/A |

### ASCII (`0x51` session, `0x52` all)

```bash
printf '\x51' | socat - UNIX-CONNECT:/run/power_ledger.sock
printf '\x52' | socat - UNIX-CONNECT:/run/power_ledger.sock
```

Example response:

```text
status=discharging pct=74 watts=-4.85 regime=power-save rpm=1800 session_mins=61 ts=123456 remaining=5h 12m
```

`session_mins`, `remaining`, and `to_full` mirror the JSON fields. `ts` is monotonic uptime seconds from the cached record.

### netcat alternative

If `socat` is not installed:

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

## Troubleshooting

| Symptom | Check |
|---------|-------|
| Service fails to start | `journalctl -u power_ledger.service -e` |
| No socket file | Confirm daemon is running; socket is created at startup |
| Permission denied on socket | Socket mode is `0666`; verify SELinux context if applicable |
| `session_mins` should reset on AC | Use `0x4A` / `0x51` (session mode), not `0x4B` / `0x52` |
| `session_mins` too low on battery | Try `0x4B` / `0x52` or `bat-time --all` for full history |
| `session_mins` stuck after plugging in | Redeploy daemon; session mode (`0x4A`/`0x51`) reports `0` on AC |
| Empty IPC response | Redeploy daemon; valid requests must always return a line |
| Missing `regime` in IPC output | Redeploy daemon; fallback still includes `regime=balanced` |
| Empty `bat-time` output | Ledger may be empty or unreadable; try `bat-time --all` |
| D-Bus warning at startup | Sleep tracking requires system bus access; service runs without it if unavailable |

## Further Reading

- [Hardware sysfs reference](technical_reference/01_hardware_sysfs.md)
- [D-Bus lifecycle](technical_reference/02_dbus_lifecycle.md)
- [Binary serialization](technical_reference/03_binary_serialization.md)
- [IPC protocol reference](technical_reference/04_ipc_json_protocol.md)
- [Riemann integration math](technical_reference/05_riemann_math.md)
