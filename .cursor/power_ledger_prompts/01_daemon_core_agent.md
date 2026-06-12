01_daemon_core_agent.md (Core Daemon Engine Specification)

This prompt targets the implementation of the main asynchronous multiplexer loop and low-level hardware polling subsystems for power_ledger_d. You must implement this engine using native Linux systems calls, ensuring zero CPU polling overhead and absolute minimal memory footprints.

# 1. System Components to Implement
You are responsible for writing and unifying the following code files:

- src/daemon/main.c: The master setup, argument parsing, initialization, and primary event loop execution.
- src/daemon/sysfs_poll.c: The dedicated hardware extraction routines that interface directly with the kernel paths.

# 2. Technical Requirements
## 2.1 The epoll Multiplexer Core

- Initialize a non-blocking asynchronous event monitor using epoll_create1(0).
- Configure a native Linux timerfd_create(CLOCK_MONOTONIC, 0) instance. Set the timer interval via timerfd_settime to fire a heartbeat tick exactly every 60 seconds. Add its file descriptor to the epoll registration pool.
- Open a read-only, non-blocking file descriptor pointing directly to /sys/class/power_supply/AC0/online (fallback to /sys/class/power_supply/ADP1/online if AC0 is missing). Register this file descriptor with the epoll structure utilizing edge-triggered mode (EPOLLET | EPOLLIN) to capture real-time power cord connection and disconnection modifications instantly.
- Build the primary execution loop around a continuous epoll_wait() blocking call. The daemon must sleep inside kernel space and consume 0% CPU until either the 60-second timerfd fires (EV_TICK) or the AC state file descriptor triggers an edge change (EV_PLUG / EV_UNPLUG).

## 2.2 Hardware Intercept Engine (sysfs Architecture)

When an event wakes the epoll loop, you must safely read raw text metrics from the kernel, parse them into integers, and pass them to the binary data layer:

- Power Consumption Management: Read /sys/class/power_supply/BAT0/power_now. Strip trailing newlines and parse the text into a raw microwatt value. Cross-reference this reading by parsing /sys/class/power_supply/BAT0/status. If the status string equals "Discharging", multiply the microwatt value by -1 to explicitly store it as a negative integer. If it reads "Charging" or "Full", leave it positive.

- Fan Telemetry Extraction: Loop through the system hardware monitors via /sys/class/hwmon/hwmon*/name. Find the index directory where the contents match "asus" or "coretemp". Cache this path and read the raw fan speed string from fan1_input. Convert it directly into a standard unsigned integer representing current RPM.

- Power Regime Evaluation: Read the system's performance preference via /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference or /sys/firmware/acpi/platform_profile. Map the text string to the correct byte enum value specified in ledger_format.h:

   - "performance" / "default" -> REG_PERFORMANCE (1)

   - "balanced" / "balance_performance" -> REG_BALANCED (2)

   - "power" / "balance_power" -> REG_POWER_SAVE (3)

   - "quiet" / "low-power" -> REG_QUIET (4)

## 2.3 Atomic Hardware Debouncing
- Maintain a static memory variable tracking the CLOCK_MONOTONIC_RAW microsecond timestamp of the last processed power state switch.

- If an edge-triggered AC epoll event fires within 3000 milliseconds of an identical event type, classify it as contact line jitter and drop the duplicate record before it hits the file system logger.

# 3. Verification & Guardrails

- Zero Polling Rule: Do not use sleep(), usleep(), or tight tracking loops. If strace shows the daemon waking up outside of your 60-second heartbeat or direct physical hardware interruptions, reject the build.

- Resource Leak Isolation: Ensure all file descriptors opened within the hardware sampling functions are closed immediately after reading (close()). Do not leak open file handlers across loop iterations.

## 4. Execution Command for Cursor
When this prompt is attached to Cursor Composer along with the orchestrator, use this prompt context to generate the complete asynchronous core infrastructure.