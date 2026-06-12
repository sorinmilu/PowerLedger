This file isolates the validation suite, allowing you to simulate fake hardware states, trigger line noise, inject massive sleep durations, and stress-test the entire C code ecosystem safely in user space without needing root privileges or risking system destabilization.

# 06_test_harness_agent.md (Virtual Hardware Simulator & Validation Suite)

This prompt targets the creation of a standalone testing infrastructure and validation harness (`power_test_suite`) for the power ledger project. The test suite must bypass live root-only Linux kernel filesystems, allowing developers to execute end-to-end regression tests, inject hardware failure flags, and verify mathematical integration bounds completely in user space.

## 1. System Components to Implement

You are responsible for writing and running the following testing module files:

- `tests/hardware_mock.c`: A virtual framework that simulates files inside a local workspace folder structure (e.g., `./mock_sys/`) to mimic kernel behavior.
- `tests/runner.c`: The master automation harness that populates fake logs, fires mock D-Bus transactions, pumps bytes into the IPC socket, and asserts correct behavior.

## 2. Technical Requirements

### 2.1 Virtual sysfs Sandbox Environment

- Instead of letting the daemon read absolute system paths directly during test runs, implement a compile-time preprocessing override flag (e.g., `-DTEST_MODE`).
- When `-DTEST_MODE` is active, redirect the internal daemon file reading paths from `/sys/class/power_supply/...` to a local workspace path like `./mock_sys/`.
- Write functions inside the test harness that dynamically write string values to these files during testing:
  - Alter `./mock_sys/power_supply/BAT0/power_now` to mock variable power drops (e.g., writing `"15000000"` for a heavy 15W load).
  - Alter `./mock_sys/power_supply/AC0/online` between `"1"` and `"0"` to verify edge-triggered connection handling.

### 2.2 Core Assertion Testing Matrices

The test runner must execute three strict validation checks:

- **The Debounce Test:** Rapidly flip the value inside `./mock_sys/power_supply/AC0/online` between `1` and `0` five times within 50 milliseconds. Read the generated binary output file and assert that **only one single event** was written to disk, confirming the 3000ms jitter trap works perfectly.
- **The Riemann Zero-Volume Test:** Inject two sequential metrics entries with an interval gap of 4 hours, but flag the first entry as an `EV_SLEEP` record. Assert that the `bat-time` calculation loop evaluates the net energy consumed during that 4-hour window as **exactly 0.00 Wh**, confirming sleep gaps are successfully ignored.
- **The Leak Profiler:** Run the daemon executable through `valgrind --leak-check=full --error-exitcode=1` while making 50 consecutive client calls to the UNIX socket endpoint. Assert that the exit code is 0, confirming there are no file descriptor leaks or memory management errors.

## 3. Verification & Guardrails

- **Zero Production Footprint:** The execution code for these mocks must be locked completely behind compiler optimization flags (`#ifdef TEST_MODE`). None of the virtual path generation scripts or testing arrays may be compiled into the final production service binary.
- **Self-Cleaning Environment:** When the testing harness completes or aborts due to an assertion failure, it must invoke cleanup commands to scrub `./mock_sys/` and any temporary binary log dumps, leaving the git tree pristine.

## 4. Execution Command for Cursor

When this prompt is linked inside Cursor Composer alongside the project orchestrator, utilize this blueprint to build the complete isolated environment simulation suite and complete the full testing pipeline.

**Commit Target:** `git commit -m "test(harness): implement virtual sysfs sandbox environments and regression assertion matrices"`