# 02_dbus_signals_agent.md (D-Bus Signal Interceptor Specification)

This prompt targets the implementation of the system suspend and resume tracking engine for `power_ledger_d`. This module connects to the low-level system D-Bus ring buffer to capture system-wide sleep transitions emitted by `systemd-logind`. This ensures our Riemann math engines can cleanly calculate accurate active time intervals without counting frozen sleep durations.

## 1. System Components to Implement

You are responsible for writing or expanding the following code modules:

- `src/daemon/dbus_handler.c`: Contains the D-Bus connection initialization, signal filter subscription, and asynchronous signal processing callback handlers.
- Integrate hooks inside `src/daemon/main.c` to add the active D-Bus socket connection file descriptor into our master `epoll` registration loop.

## 2. Technical Requirements

### 2.1 Low-Level D-Bus Connection Lifecycle

- Establish a connection to the system-wide message bus by executing `dbus_bus_get(DBUS_BUS_SYSTEM, &error)`. Handle initialization failures gracefully by printing explicit error diagnostics to `stderr` and safely backing out.
- Configure the connection to run in a non-blocking mode. Extract the raw underlying socket file descriptor using `dbus_connection_get_unix_fd()` or use `dbus_connection_set_watch_functions()` to monitor read/write readiness. Register this socket descriptor directly with the central `epoll` instance handled by `01_daemon_core_agent.md` using `EPOLLIN`.

### 2.2 Systemd Suspend and Resume Interception

- Register a concrete D-Bus match rule filtering for the `PrepareForSleep` signal. The match string must precisely define these constraints:
  Plaintext
  ```
  type='signal',sender='org.freedesktop.login1',interface='org.freedesktop.login1.Manager',member='PrepareForSleep',path='/org/freedesktop/login1'

  ```
- Inside the signal reception loop/callback, unpack the incoming message payload. The `PrepareForSleep` signal provides a single boolean parameter:
  - `true` **(System entering sleep):** Immediately pause standard metric gathering, populate a packed structure with current stats, force the event type to `EV_SLEEP` (3), and execute a blocking, atomic flush sequence directly into `/var/log/power_ledger.bin`.
  - `false` **(System resuming from sleep):** Instantly read the fresh hardware registers upon wake, populate the packed structure, force the event type to `EV_WAKE` (4), and append it directly to the binary log file.

### 2.3 Resource Cleanup & Fault Isolation

- Provide a clean teardown function `dbus_handler_shutdown()` that removes the match rule, closes the active connection via `dbus_connection_unref()`, and removes its file descriptor footprint from the `epoll` monitoring cluster during daemon termination.

## 3. Verification & Guardrails

- **Zero-Alloc Callback Path:** The callback code path running during a `PrepareForSleep(true)` notification must execute instantly. Avoid heavy allocations or complex library calls; write directly to the persistent binary log descriptor before the kernel suspends the system process scheduler.
- **No libdbus-glib/GIO Bloat:** Use the raw, low-level `libdbus-1` native C headers (`#include <dbus/dbus.h>`). Do not introduce GLib, GObject, or any heavy desktop-environment utility dependencies. Keep it ultra-lean.

## 4. Execution Command for Cursor

When this prompt is passed to Cursor Composer along with the orchestrator blueprint, use this context to construct the complete asynchronous D-Bus signal trap architecture and tie it into the core `epoll` engine.

**Commit Target:** `git commit -m "feat(signals): implement dbus system suspend listeners for pre/post sleep milestones"`