This prompt isolates the math logic—the Piecewise Left-Hand Riemann Sum integration—ensuring the parser handles system sleep intervals accurately without corrupting your tracking data.

# 05_cli_client_agent.md (Computational Engine & CLI Parser Specification)

This prompt targets the implementation of the `bat-time` command-line analytics consumer tool. This application reads the raw binary append log at `/var/log/power_ledger.bin`, parses the sequential 16-byte packed records, filters out system suspend/sleep dead zones, and executes a piecewise numerical integration loop to output true, highly accurate energy efficiency metrics.

## 1. System Components to Implement

You are responsible for writing and optimizing the following client-side module:

- `src/client/bat_time.c`: A standalone, lightweight executable that hooks into the storage binary layer, computes active efficiency deltas, and prints clean formatting charts directly to the user's shell terminal.

## 2. Technical Requirements

### 2.1 The Piecewise Left-Hand Riemann Integration Algorithm

To evaluate cumulative battery energy drain (mWh or Wh) accurately without relying on databases, you must reconstruct history by stepping through the raw data sequentially. For each interval between record $i$ and record $i+1$:

- **Calculate Time Delta:**
  $$\Delta t = t_{i+1} - t_{i}$$
  where $t$ is the `timestamp` field containing monotonic kernel seconds (`CLOCK_MONOTONIC_RAW`).
- **Handle Sleep Gaps:** If the record at index $i$ has an event type equal to `EV_SLEEP` (3), or the record at index $i+1$ has an event type equal to `EV_WAKE` (4), **you must explicitly force $\Delta t = 0$ for that segment.** This strips away the giant wall-clock gaps when the laptop was closed and suspended.
- **Accumulate Energy Consumption:** If the system was on battery during the interval (indicated by a negative `power_drain` value), accumulate the total power segment slice:
  $$\text{Segment Energy (Wh)} = \frac{|\text{power\_drain}_{i}|}{1,000,000} \times \frac{\Delta t}{3600.0}$$
- Divide by 1,000,000 because `power_drain` is stored as micro-watts ($\mu\text{W}$). Sum all valid segments across the target boundary limits to compute total session active drain.

### 2.2 Backward File Scanning Stream

- To keep execution speeds instantaneous on large log histories, the client must open `/var/log/power_ledger.bin` and read the structures from the tail backward using low-level file seeking (`lseek`).
- If the user supplies no terminal flags, search backward until hitting the most recent `EV_WAKE` or `EV_UNPLUG` sequence that marks the beginning of the current active runtime session block. Stop processing older records once this boundary is identified, unless the user provides an explicit `--all` flag to compute history limits across weeks.

### 2.3 Text UI Layout Formatting

- Print the calculated results to the terminal interface in a crisp, minimalist, high-scannable plaintext block. Avoid heavy libraries or complex output structures.
- The output block must explicitly present:
  - Total active session runtime duration (rendered cleanly as hours and minutes, completely omitting sleep dead zones).
  - Net Energy Consumed (expressed in standard Watt-hours, Wh).
  - Average Session Burn Rate (expressed in Watts, e.g., `4.35W`).

## 3. Verification & Guardrails

- **The Frozen Clock Test Matrix:** You must provide a self-contained code verification loop that feeds a mock timeline array to your mathematical functions. This test timeline must contain 2 hours of active draining at a constant 5.0W, followed by a 24-hour sleep gap (`EV_SLEEP` to `EV_WAKE`), followed by another 1 hour of draining at 5.0W. Your math engine must evaluate the total active duration as exactly 3 hours (not 27 hours) and total energy drain as exactly 15.0 Wh.
- **Memory Constancy Constraints:** Do not read the entire log file directly into a giant, dynamically allocated array in memory. Stream the data records from the file descriptor sequentially using fixed-size lookback chunks (e.g., parsing blocks of 64 records at a time) to maintain a flat, near-zero RAM execution envelope.

## 4. Execution Command for Cursor

When this sub-prompt is fed into Cursor Composer alongside the project's orchestrator blueprint, use this context to craft the standalone computational parsing engine and tie it tightly to the binary logging files.

**Commit Target:** `git commit -m "feat(ipc): complete riemann calculation tool and unix domain socket interface loops"`