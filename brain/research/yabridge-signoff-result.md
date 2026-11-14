# Yabridge appliance sign-off: result record

Status: **not run, needs UMC1820 appliance.**

The sign-off is the run of `tools/signoff-yabridge.sh` on the installed
appliance, with the Behringer UMC1820 attached and a real Windows VST3 bridged
through yabridge. The criteria are ADR-0139. Nothing in this file is a
measurement: the results table stays empty until that run is made. The hardware
part is parked to the end by the owner (2026-11-13), so this record is the
template the run fills in, not a pass.

## Commands

Run on the appliance, not in a test prefix (the script uses the caller's real
`~/.vst3/yabridge`):

```
tools/signoff-yabridge.sh --host HOST --driver DRIVER --plugin PATH.vst3 \
    --blocks 225000 --report FILE
```

- `HOST`: the built `sonora-plugin-host`, executable.
- `DRIVER`: the built `plugin_driver`, executable.
- `PATH.vst3`: the real Windows VST3 to bridge.
- `--blocks 225000`: a 10-minute run at 128 frames, 48 kHz (600 s / 2.667 ms).
  The script's default of 20000 blocks is about 53 s and is not a sign-off run.
- `--report FILE`: where the criteria report is written (also printed).
- Environment: `SONORA_YABRIDGE_TEST_VST3_DIR` must hold `sonora-test-crash.vst3`
  (the Windows build of the crash test plugin).

Required on PATH: `wine`, `wineserver`, `yabridgectl`, `yabridge-host.exe`.
Exit codes: 0 every criterion holds; 1 a criterion fails; 77 Wine, yabridge or
the plugin is absent (skipped, not a pass).

The evaluator is exercised without Wine by `tools/test-signoff-yabridge.sh`,
which feeds it fixtures. A green fixture test is not the sign-off.

## Criteria (ADR-0139, Decision)

Measured on the installed appliance, real period (128 frames, 48 kHz), 10-minute
run.

| # | Criterion | Value | Constant in `tools/lib/yabridge-signoff.sh` |
|---|---|---|---|
| 1 | Round trip p99.9 inside the deadline | at or below **2000 us** | `YB_DEADLINE_US` = 2000 |
| 2 | Worst block (max round trip) inside the period | at or below **2667 us** | `YB_PERIOD_US` = 2667 |
| 3 | No overruns: zero blocks dropped | **0** dropped over **225000** blocks | `--blocks` default aligned to 225000 in `tools/signoff-yabridge.sh` |
| 4 | Wine audio threads at an RT class | at least one `SCHED_FIFO` or `SCHED_RR` thread with `rtprio` > 0 in `ps -eLo cls,rtprio,comm` | `yb_wine_rt_count` |
| 5 | Crash recorded and contained | `crash_bridged` ends `died` or `silent`, callbacks ran, driver exits 0 | `yb_crash_outcome`, `yb_evaluate` |

The margin between criteria 1 and 2 is 2667 - 2000 = 667 us.

Criterion 4 is an assumption to settle by the hardware run, as ADR-0139 says.
Criterion 5 records the outcome (`died` or `silent`) and does not judge it; the
criterion is that the engine survived.

## Results

Not yet run. Fill in from the report of the appliance run.

| Measure | Value | Criterion | Result |
|---|---|---|---|
| Round trip p50 (us) | | (none, recorded) | |
| Round trip p99 (us) | | (none, recorded) | |
| Round trip p99.9 (us) | | 1: at or below 2000 | |
| Round trip max (us) | | 2: at or below 2667 | |
| Blocks dropped over 225000 | | 3: 0 | |
| Wine RT-class threads (count) | | 4: at least 1 | |
| Crash scenario outcome (`died` / `silent` / `none`) | | 5: died or silent, callbacks ran | |
| Crash driver exit code | | 5: 0 | |
| Plugin, kernel, date of run | | (recorded) | |
| Overall `SIGN-OFF` line | | all five | |

## Status

- Run: **not yet run**.
- Blocked on: the UMC1820 appliance and a real Windows VST3 (hardware, parked to
  the end by the owner, 2026-11-13).
- Until a run prints `SIGN-OFF: PASS`, the "Windows (Wine)" chooser entry stays
  marked unverified (ADR-0139, Failure).
