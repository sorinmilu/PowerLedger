# 04_ipc_socket_agent.md (IPC Socket Engine & JSON Stream Protocol)

This prompt targets the implementation of the Interprocess Communication (IPC) subsystem for `power_ledger_d`. This module instantiates a localized UNIX domain socket at a system runtime path, establishes a rigid byte-driven request protocol, and delivers minified JSON metrics strings to GUI dashboard elements or plain text stats to CLI utilities.

## 1. System Components to Implement

You are responsible for writing or expanding the following system modules:

- `src/daemon/ipc_socket.c`: The background listener loop that initializes the socket endpoint, registers it into the primary `epoll` multiplexer, and services incoming client connections.
- Update `src/daemon/main.c` to handle smooth cleanup and unlinking of the socket file when the system stops.

## 2. Technical Requirements

### 2.1 UNIX Domain Socket Lifecycle

- Initialize an IPC server socket using `socket(AF_UNIX, SOCK_STREAM, 0)`.
- Bind the socket endpoint to the absolute runtime file layer path: `/run/power_ledger.sock`.
- Set the socket to non-blocking mode (`O_NONBLOCK`) and append its file descriptor to the master background `epoll` instance handled by `01_daemon_core_agent.md` monitoring for `EPOLLIN`.
- **Access Control & Permissions:** Immediately after binding, alter the file access bits using `chmod("/run/power_ledger.sock", 0666)`. This explicitly allows unprivileged graphical desktop environments or menu bar widgets to read the stream while the underlying daemon maintains its root privileges.

### 2.2 Wire Protocol and Micro-Transactions

When a client connects to the socket, the listener must expect a single-byte request transaction header before replying:

- **CLI Query Header (**`0x51`**):** The daemon reads the internal memory cache of the latest `PowerLedgerEvent` tracking structure and responds by streaming a raw, structured binary block or a clean plain-text layout.
- **Widget JSON Header (**`0x4A`**):** The daemon intercepts this byte and immediately serializes the latest live system metrics into a raw, single-line, minified JSON text payload string. The string layout must look exactly like this, terminated with a clean newline character (`\n`):
  JSON
  ```
  {"status":"discharging","pct":74,"watts":-4.85,"regime":"power-save","rpm":1800,"session_mins":142}

  ```
- Close the client connection socket immediately after pushing the requested payload to ensure the file descriptor pool is kept fully open.

### 2.3 Structural Teardown & Traps

- Implement an explicit signal trapping routine for `SIGINT` and `SIGTERM`.
- When the daemon terminates, it must gracefully drop out of its `epoll` run loop, close its socket handle, and invoke `unlink("/run/power_ledger.sock")` to clear the node from the runtime file system.

## 3. Verification & Guardrails

- **The Microsecond Constraint:** Processing a single transaction payload must use an absolute minimum of CPU cycles. The daemon must fetch values directly from its local RAM cache updated by the `epoll` thread, rather than invoking heavy disk-read loops on `/var/log/power_ledger.bin` during a live client stream.
- **No Dynamic Memory Accumulation:** Do not use `malloc()` loops inside the client processing callbacks. Use localized fixed-width text string buffers (`char buffer[512]`) to construct JSON streams to prevent memory leaks or runtime fragmentation.

## 4. Execution Command for Cursor

When this specification prompt is called inside Cursor Composer alongside the orchestrator blueprint, use this system framework to implement the underlying non-blocking UNIX domain socket protocol and bind it cleanly into the active server loop.

**Commit Target:** `git commit -m "feat(ipc): complete riemann calculation tool and unix domain socket interface loops"`