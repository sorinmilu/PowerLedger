# 08_documentation_agent.md (Documentation Pipeline & System Reference Specification)

This prompt targets the automatic generation, structural organization, and continuous synchronization of the project documentation ecosystem. The documentation must be kept in a dedicated top-level folder layout called `docs/`, completely separated into production user-facing manuals and low-level technical system API architecture references.

## 1. System Documentation Structure

You are responsible for generating, reviewing, and continuously updating the following markdown document matrices:

## 1. System Documentation Structure

You are responsible for generating, reviewing, and continuously updating the following markdown document matrices:
* `docs/user_manual.md`: Designed for users/sysadmins looking to deploy the service or parse socket data for widgets.
* `docs/technical_reference/01_hardware_sysfs.md`: Maps kernel paths, fan telemetry extraction, and microsecond throttling logic.
* `docs/technical_reference/02_dbus_lifecycle.md`: Maps out the raw systemd-logind subscription rules and suspend state traps.
* `docs/technical_reference/03_binary_serialization.md`: Standardizes the exact bitwise layout table for the 16-byte packed log records.
* `docs/technical_reference/04_ipc_json_protocol.md`: Documents the wire protocol byte codes (0x51/0x4A) and the minified JSON schema.
* `docs/technical_reference/05_riemann_math.md`: Contains the formal mathematical pseudocode for calculations across sleep dead-zones.


## 2. Technical Requirements

### 2.1 User-Facing Dashboard Documentation (`user_manual.md`)

This document must provide a straightforward, step-by-step framework to deploy, interact with, and verify the power tracker service. It must explicitly document:

- **Installation Protocol:** Clear commands to download system headers on Fedora using `dnf5`, execute `make`, and copy the compiled binary into system binary layers.
- **Systemd Integration Management:** A complete, valid production `power_ledger.service` unit configuration template file block so users can execute `systemctl enable --now power_ledger` to run it as a permanent system background service.
- **The IPC Interface Specification:** Clear examples of how an unprivileged desktop tool or dashboard status bar (like Waybar or polybar) can poll `/run/power_ledger.sock` using standard tools like `socat` or `netcat`, explicitly mapping out the exact minified JSON response keys and value units.

### 2.2 Deep-Dive Engineering Architecture Reference (`technical_reference.md`)

This document must serve as the absolute low-level truth for systems engineers tracking code evolution. It must explicitly map out:

- **Binary File I/O Array Schema:** A markdown layout table detailing the bitwise structure of the 16-byte packed log records, explicitly documenting byte-offsets, data sizes, enum bitmask definitions, and value scales (e.g., explaining that `power_drain` is a signed field measured in micro-watts).
- **Asynchronous Signal Flows:** An exhaustive sequence chart mapping how the internal `epoll_wait()` engine responds to timer wakeups, hardware edge events via sysfs, and `libdbus` runtime blocks when capturing a system `PrepareForSleep` notification thread.
- **The Riemann Sum Computation Boundary:** Rigorous mathematical pseudo-code breaking down how the `bat-time` parser scans raw bytes backward from file tips, calculates piecewise integrals, and isolates sleep dead-zones to arrive at an accurate energy consumption figure.

### 2.3 Continuous Synchronization Guardrails

- **The Code-to-Doc Audit Protocol:** When any sub-agent updates an architectural layout (such as changing an enum integer value, shifting a sysfs tracking path, or modifying a JSON output key), this agent must immediately scan those code variations and patch the corresponding section in `docs/technical_reference.md`.
- **Zero Out-of-Sync Threshold:** Documentation must never contain speculative explanations. Every command sequence, code sample, or configuration block mapped inside `docs/` must compile and execute successfully against the current system state.

## 3. Verification & Guardrails

- **No Code Bloat in Documentation:** Avoid wrapping giant copies of the production C files inside the documentation markdown. Use clean, highly distilled code snippets, structure definitions, and tables to explain architecture behavior.
- **Absolute Path Constraints:** Explicitly ensure all local filesystem paths reference standard system paths (`/var/log/...`, `/run/...`) or relative repository paths (`./src/`), avoiding absolute hardcoded developer directories.

## 4. Execution Command for Cursor

When this specification prompt is issued to Cursor Composer alongside our master orchestrator blueprint, verify that the `docs/` directory is clean, structure the markdown blueprints, and sync them perfectly with our active code layout.

**Commit Target:** `git commit -m "docs(manuals): initialize user installation blueprints and low-level technical API references"`

### Syncing the Project Master Orchestrator

To complete your system framework, append this new asset to the project directory structure inside your primary orchestrator file (`00_master_blueprint.md`):

Plaintext

```
├── .cursor/
│   └── power_ledger_prompts/
│       ├── 07_build_toolchain_agent.md <-- Targets build system & Makefile switches
│       └── 08_documentation_agent.md   <-- Targets automated user & technical manuals
├── docs/
│   ├── user_manual.md                 <-- User installation & systemd configurations
│   └── technical_reference.md         <-- Packed structs bitwise layouts & API references

```

