# ADR-0173: Graph benchmark, 18 tracks x 4 plugins at quantum 128

Status: accepted (architect, 2026-11-08)

## Summary
`plugins.graph.benchmark` is closed by `sonora-graph-bench`, a tool that
drives `Session::process()` at quantum 128 with 18 tracks of 4 plugins each
and one 2048-frame track beside dry ones. CI gates on counts (zero drops,
zero allocations, alignment bit for bit), never on time. The time budget is
a verdict the tool prints under `--strict-time`; the laptop run is recorded
in the PR, the UMC1820/RT-kernel rerun belongs to OS packaging.

## Context
The 2048-sample alignment half of the exit is already on main
(`test_plan_pdc`, `session_plugins_aligned_2048`). Missing: the load half.
Testing rules forbid wall-clock assertions (flakes 2026-11-04/06), but the
claim "18 x 4 at q128 fits" is a time claim.

## Decision
0. Home: engine/core/bench, not engine/plugin_client: the tool drives
   `Session`, and plugin_client -> core is a forbidden edge (ADR-0007);
   core -> plugin_client is allowed. module-deps.allow is untouched.
   `--fake` reads an injected `FakeClock` that moves only when the injected
   plugin stalls, so the fake run counts misses exactly, with no wall clock.
1. Tool `sonora-graph-bench` (engine/core/bench/): builds a session
   of 18 tracks, each with a 4-plugin chain (in-tree native plugins through
   the real host, class from ADR-0011), plus the 2048-frame aligned case;
   runs N callbacks (default 20000) at quantum 128, 48 kHz (budget 2.667 ms).
2. It prints one `RESULT` line: callbacks, p50/p99/max callback time, misses
   (callback over budget), host-miss and drop counters (ADR-0063), allocation
   count, alignment lag. Exit 1 if drops, allocations or lag are non-zero, or
   (with `--strict-time`) if any callback is over budget.
3. Ctest `plugin_client_bench_smoke` (label `core`) runs the tool with a
   fake runner: asserts counts, alignment and the RESULT format, not time.
   A real-host run is `plugin_client_bench_real`, label `timing`,
   RUN_SERIAL.
4. No protocol, no format, no engine behaviour change. If the bench exposes
   a miss the fix is a separate item, not part of this one.
5. The item is ticked on a laptop PASS run pasted in the PR; the hardware
   rerun stays a note on OS packaging.

## Rationale
Counts are deterministic; time is not. Splitting the verdict keeps CI
honest without a flaky gate, and still gives a number a human reads.

## Consequences
Cost: a tool and two tests to keep. A laptop number is not hardware proof;
it is stated as such.

## Unverified
Whether 18 x 4 fits at q128 on the laptop and on the UMC1820 with the RT
kernel is an assumption until the researcher runs `--strict-time` on both;
record the p99 and the plugin classes used. The in-tree plugin mix chosen
for "4 plugins" (EQ, compressor, delay, reverb) is a default.

## Alternatives rejected
- Gate CI on the callback time: flakes on a shared runner, violates
  testing-and-flakes.
- Only extend `test_session_plugins`: no timing output, nothing to rerun on
  hardware.
- Close by reference to the 2048 test: leaves the 18 x 4 load claim unproved.
