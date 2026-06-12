00_master_blueprint.md (Project Orchestrator)

This document serves as the absolute architectural root for the power_ledger implementation. Cursor Agents must read this file first, map the directory structure, and refer to the specific sub-markdown prompts referenced in Section 3 for individual module implementation details.

# 1. System Architecture Diagram

```
          +-------------------------------------------------------+
          |                 Linux Kernel / D-Bus                  |
          +-------------------------------------------------------+
                | (sysfs telemetry)     | (timerfd)         | (D-Bus signal)
                v                       v                   v
          +-------------------------------------------------------+
          |             power_ledger_d (Core Daemon)              |
          |  - Monitors /sys/class/power_supply and /sys/class/hwmon|
          |  - Catch system sleep milestones asynchronously       |
          |  - Append-only atomic writes to /var/log/power_ledger.bin|
          +-------------------------------------------------------+
                                        |
                             [/run/power_ledger.sock]
                                        |
                  +---------------------+---------------------+
                  |                                           |
                  v                                           v
    +-------------------------------+           +-----------------------+
    |     bat-time (CLI Client)     |           |   Desktop Bar Widget  |
    | - Riemann integration parser  |           | - Polls IPC Socket    |
    | - Subtracts sleep dead-zones  |           | - Consumes JSON stream|
    +-------------------------------+           +-----------------------+
```

# 2. Directory & Prompt Tree Specification

The repository workspace must match this precise structural layout. Each code module corresponds directly to a dedicated instruction prompt file located in .cursor/power_ledger_prompts/.

```

├── .cursor/
│   └── power_ledger_prompts/
│       ├── 00_master_blueprint.md     <-- This file (Project Orchestrator)
│       ├── 01_daemon_core_agent.md    <-- Targets epoll loop, timerfd, & sysfs engine
│       ├── 02_dbus_signals_agent.md   <-- Targets systemd suspend/wake listeners
│       ├── 03_binary_io_agent.md      <-- Targets packed struct operations & file logs
│       ├── 04_ipc_socket_agent.md     <-- Targets UNIX socket management & JSON protocols
│       ├── 05_cli_client_agent.md     <-- Targets Riemann interval calculations
│       ├── 06_test_harness_agent.md   <-- Targets mock telemetry & validation validation
│       ├── 07_build_toolchain_agent.md <-- Targets build system & Makefile switches
│       └── 08_documentation_agent.md  <-- Targets automated user & technical manuals
├── docs/
│   ├── user_manual.md                 <-- User installation, CLI usage, & systemd service
│   └── technical_reference/            <-- Split Technical Architecture Manuals
│       ├── 01_hardware_sysfs.md       <-- Kernel paths, fan indexes, & debounce rules
│       ├── 02_dbus_lifecycle.md       <-- PrepareForSleep state transitions & signals
│       ├── 03_binary_serialization.md <-- 16-byte packed struct layout & fsync rules
│       ├── 04_ipc_json_protocol.md    <-- UNIX socket byte headers & JSON schemas
│       └── 05_riemann_math.md         <-- Piecewise Left-Hand integration calculus
├── src/
│   ├── daemon/
│   │   ├── main.c
│   │   ├── sysfs_poll.c
│   │   ├── dbus_handler.c
│   │   ├── binary_io.c
│   │   └── ipc_socket.c
│   ├── shared/
│   │   └── ledger_format.h
│   └── client/
│       └── bat_time.c
├── tests/
│   ├── hardware_mock.c
│   └── runner.c
└── Makefile
```

## 3. Sub-Prompt Reference Matrix

When implementing, modifying, or debugging a specific layer of the system, Cursor Agents must load the corresponding sub-prompt file to preserve strict scope isolation and prevent code regressions:

- **For the primary asynchronous core loop:** Refer to `.cursor/power_ledger_prompts/01_daemon_core_agent.md`. This prompt dictates how the `epoll_create1` and `timerfd` heartbeat engines handle non-blocking sysfs lookups.
- **For handling system-wide sleep states:** Refer to `.cursor/power_ledger_prompts/02_dbus_signals_agent.md`. This prompt isolates the low-level `libdbus` integration blocks to capture async `PrepareForSleep` transitions cleanly.
- **For validating storage files and allocations:** Refer to `.cursor/power_ledger_prompts/03_binary_io_agent.md`. This prompt enforces the strict 16-byte packed structure boundaries, crash resilience, and binary file logs.
- **For modifying the communication streams:** Refer to `.cursor/power_ledger_prompts/04_ipc_socket_agent.md`. This prompt isolates the UNIX domain socket backend listener configurations and minified JSON protocol formats.
- **For modifying numerical integration analytics:** Refer to `.cursor/power_ledger_prompts/05_cli_client_agent.md`. This prompt controls the standalone `bat-time` terminal utility and its Piecewise Left-Hand Riemann Sum integration calculus.
- **For verifying environmental simulation states:** Refer to `.cursor/power_ledger_prompts/06_test_harness_agent.md`. This prompt builds the unprivileged user-space testing sandbox, virtual hardware trees, and jitter assertion checks.
- **For managing compilation and linker behavior:** Refer to `.cursor/power_ledger_prompts/07_build_toolchain_agent.md`. This prompt standardizes our strict `-std=c11` CFLAGS compilation standards, `-Werror` safety loops, and DNF5 dependency matching.
- **For coordinating document synchronizations:** Refer to `.cursor/power_ledger_prompts/08_documentation_agent.md`. This prompt drives the automated pipeline that updates user installation manuals and modular architecture deep-dives simultaneously.

# 4. Multi-Phase Atomic Commit Workflow

The project implementation is structured into sequential development milestones. Each phase requires completing the corresponding functional code and verifying documentation blocks before proceeding to the next layer:

## Phase 1: Environment & Serialization Core (Data Layer)

Target Guidelines: 03_binary_io_agent.md, 07_build_toolchain_agent.md, and 08_documentation_agent.md

Execution: Create the centralized compilation Makefile and initialize src/shared/ledger_format.h to enforce precise byte-aligned memory layouts. Generate the foundation file docs/technical_reference/03_binary_serialization.md.

Commit Milestone: git commit -m "build(infra): implement strict C11 toolchain matrix and packed binary ledger structures"

## Phase 2: Multiplexer Core & Simulation Sandbox (Multiplexer Layer)

Target Guidelines: 01_daemon_core_agent.md, 06_test_harness_agent.md, and 08_documentation_agent.md

Execution: Initialize the main epoll_create1 and timerfd asynchronous event infrastructure. Hook the low-level /sys/class/ hardware monitoring lookups. Implement the compile-time -DTEST_MODE user-space override path and generate docs/technical_reference/01_hardware_sysfs.md.

Commit Milestone: git commit -m "feat(daemon): initialize epoll multiplexer core and mount virtual hardware simulation sandbox"

## Phase 3: D-Bus Lifecycle Management (Signal Layer)

Target Guidelines: 02_dbus_signals_agent.md and 08_documentation_agent.md

Execution: Integrate the non-blocking libdbus wire connection into the core epoll listener ring. Trap the active PrepareForSleep boolean state transitions to handle EV_SLEEP and EV_WAKE sequences cleanly. Generate docs/technical_reference/02_dbus_lifecycle.md.

Commit Milestone: git commit -m "feat(signals): integrate native systemd-logind dbus suspend and resume traps"

## Phase 4: IPC Interface & Numerical Analytics (IPC & Math Layer)

Target Guidelines: 04_ipc_socket_agent.md, 05_cli_client_agent.md, and 08_documentation_agent.md

Execution: Open the non-blocking UNIX domain socket server at /run/power_ledger.sock handling 0x51 ASCII and 0x4A minified JSON streams. Write the standalone bat-time consumer utility utilizing Piecewise Left-Hand Riemann Sum logic to prune sleep gaps. Generate docs/technical_reference/04_ipc_json_protocol.md and docs/technical_reference/05_riemann_math.md.

Commit Milestone: git commit -m "feat(ipc): complete riemann calculus analytics tool and non-blocking unix socket json stream"

## Phase 5: Quality Assurance, Security, & Deployment (Verification Layer)

Target Guidelines: 06_test_harness_agent.md and 08_documentation_agent.md

Execution: Run the final assertion suites to verify the 3000ms hardware debounce jitter trap and the frozen sleep math clock rules. Profile the daemon under valgrind across 50 simulated socket connections to guarantee a 0-byte memory leak footprint. Compile the definitive user-facing deployment guide at docs/user_manual.md containing the native systemd unit configuration file.

Commit Milestone: git commit -m "test(ci): run end-to-end sandbox regression tests and ship systemd service configurations"