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

## Binary Log and CLI

| Path | Purpose |
|------|---------|
| `/var/log/power_ledger.bin` | Append-only 16-byte event ledger |
| `/run/power_ledger.sock` | Live telemetry UNIX stream socket |
| `/usr/local/bin/bat-time` | Riemann energy integration CLI |

Analyze the current unplugged session (default):

```bash
bat-time
```

Integrate the full ledger history:

```bash
bat-time --all
```

Use a custom log path:

```bash
bat-time -f /var/log/power_ledger.bin
```

Example output:

```text
Active Session Runtime : 2h 15m
Net Energy Consumed    : 8.42 Wh
Average Session Burn   : 3.74W
```

## IPC Polling (Widgets and Dashboards)

Connect to `/run/power_ledger.sock`, write **one request byte**, read one response line, then disconnect.

| Request byte | Format | Example tool |
|--------------|--------|--------------|
| `0x4A` | Minified JSON + `\n` | Waybar, polybar, scripts |
| `0x51` | ASCII key/value + `\n` | Shell one-liners |

### JSON poll (`0x4A`)

```bash
printf '\x4A' | socat - UNIX-CONNECT:/run/power_ledger.sock
```

Example response:

```json
{"status":"discharging","pct":74,"watts":-4.85,"regime":"power-save","rpm":1800,"session_mins":142}
```

| Key | Type | Unit / meaning |
|-----|------|----------------|
| `status` | string | `discharging`, `charging`, or `idle` |
| `pct` | integer | Battery capacity percentage (0–100) |
| `watts` | float | Instantaneous power in watts |
| `regime` | string | `performance`, `balanced`, `power-save`, or `quiet` |
| `rpm` | integer | Fan speed |
| `session_mins` | integer | Minutes since last `EV_WAKE` or `EV_UNPLUG` |

### ASCII poll (`0x51`)

```bash
printf '\x51' | socat - UNIX-CONNECT:/run/power_ledger.sock
```

Example response:

```text
status=discharging pct=74 watts=-4.85 regime=power-save rpm=1800 ts=123456
```

`ts` is monotonic uptime seconds from the cached record.

### netcat alternative

If `socat` is not installed:

```bash
printf '\x4A' | nc -U /run/power_ledger.sock
```

Some `nc` builds require `-N` to close after EOF:

```bash
printf '\x4A' | nc -U -N /run/power_ledger.sock
```

## Troubleshooting

| Symptom | Check |
|---------|-------|
| Service fails to start | `journalctl -u power_ledger.service -e` |
| No socket file | Confirm daemon is running; socket is created at startup |
| Permission denied on socket | Socket mode is `0666`; verify SELinux context if applicable |
| Empty `bat-time` output | Ledger may have no post-boundary records; try `bat-time --all` |
| D-Bus warning at startup | Sleep tracking requires system bus access; service runs without it if unavailable |

## Further Reading

- [Hardware sysfs reference](technical_reference/01_hardware_sysfs.md)
- [D-Bus lifecycle](technical_reference/02_dbus_lifecycle.md)
- [Binary serialization](technical_reference/03_binary_serialization.md)
- [IPC JSON protocol](technical_reference/04_ipc_json_protocol.md)
- [Riemann integration math](technical_reference/05_riemann_math.md)
