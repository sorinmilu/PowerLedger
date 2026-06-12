# 07_build_toolchain_agent.md (Build System & Compilation Specification)

This prompt targets the implementation of the project automation layers, compilation switches, and dependency resolution maps for the entire `power_ledger` system. You must enforce strict compilation standards to ensure zero code bloat and absolute safety.

## 1. System Components to Implement

You are responsible for creating and maintaining the centralized project build file:

- `Makefile`: The single automation matrix used to build the production daemon, the client tool, and the testing harness.

## 2. Technical Requirements

### 2.1 Compiler Selection and Strict Warning Flags

All modules must be compiled using `gcc` under the **C11 standard** (`-std=c11`). You must enforce extreme compiler strictness to catch memory alignment or pointer type mismatches before they hit production. The base compilation flags (`CFLAGS`) must include:

- `-Wall -Wextra -Wpedantic`: Enable all standard and extra warning matrices.
- `-Werror`: Treat every single warning as a fatal compilation failure.
- `-Wstrict-prototypes -Wmissing-prototypes`: Enforce rigid function definition architectures.

### 2.2 Optimization & Footprint Trimming Switches

To honor our preference for lightweight, high-performance execution blocks with zero system bloat, compile the production binaries using these specific optimization behaviors:

- `-O2`: Enable standard high-performance optimizations without inflating the binary size.
- `-fno-plt`: Reduce dynamic linker overhead for internal function references.
- `-g`: Retain debug symbols so that if a crash occurs, `valgrind` can map lines down to the exact source byte.

### 2.3 Dependency Mapping & Linker Flags (`LDFLAGS`)

Do not hardcode library include paths. Use `pkg-config` to dynamically locate headers and link libraries on Fedora. Your linking targets must resolve exactly as follows:

- **For the Core Daemon (**`power_ledger_d`**):** Must link against the system bus architecture libraries.
  Makefile
  ```
  DBUS_FLAGS := $(shell pkg-config --cflags --libs dbus-1)

  ```
- **For the Client Tool (**`bat-time`**):** Keep it clean. It should only link against standard math libraries (`-lm`) to resolve the decimal parsing of the Riemann sums.

## 3. Makefile Architecture Reference

The `Makefile` must support the following targets out of the box:

- `make all`: Compiles both `src/daemon/` and `src/client/` to emit optimized production binaries (`bin/power_ledger_d` and `bin/bat-time`).
- `make test`: Appends the `-DTEST_MODE` preprocessing flag, compiles the local simulation suites inside `tests/`, and executes the assertion runner.
- `make clean`: Completely sweeps the workspace, unlinking any mock files, localized binary log entries, and the `bin/` directory output blocks.

## 4. Execution Command for Cursor

When this specification prompt is called inside Cursor Composer along with the orchestrator blueprint, use this context to generate the primary system `Makefile`. Ensure that running `make` runs cleanly without a single warning.

**Commit Target:** `git commit -m "build(makefile): implement centralized strict compilation toolchain automation matrix"`

### Updated Blueprint Action Plan

Now that we have this, make sure to add it to your `00_master_blueprint.md` file layout under the tree view so the orchestrator knows it exists:

Plaintext

```
│       ├── 06_test_harness_agent.md   <-- Targets mock telemetry & validation
│       └── 07_build_toolchain_agent.md <-- Targets build system & Makefile switches

```

With your tools installed via DNF5 and all 7 sub-prompts ready to go in your `.cursor/power_ledger_prompts/` directory, you have a bulletproof workspace context. You can now prompt Cursor Composer:

> `@00_master_blueprint.md @03_binary_io_agent.md @07_build_toolchain_agent.md Let's execute Phase 1. Create the` src/shared/ledger_format.h`structure, and write the minimalist`Makefile `rules to verify our binary layouts.`

