# mixassist.reverb_delay: verification of the planner on main

Date: 2026-11-09. Decision: ADR-0128 (verification only, no code change).
Delivered in #437 (`fec6609f`, bundle engine-mix). Checked against `origin/main` at `b9ba715c`.

## What was verified

- `ReverbDelayPlanner`, `ReverbDelayReference` and `propose_mix_plan(MixPlanKind::Space | All)`
  in `engine/control/include/sonora/mix_planner.h` and `engine/control/src/mix_planner.cpp`.
- Tests in `engine/control/tests/test_mix_planner.cpp`: tempo snapping of the echo time
  and tail, and the excess check (reverb plus echo power-summed against dry, lowered
  together when over `max_wet_db`).

## Command and output

Run through the machine lock, as the PR gate does for this file set:

    printf '%s\n' engine/control/tests/test_mix_planner.cpp \
      engine/control/src/mix_planner.cpp engine/control/include/sonora/mix_planner.h > mixplan-files.txt
    tools/micro-build.sh --files-from mixplan-files.txt --no-rtsan   # takes tools/build-slot.sh itself

Relevant lines of the output (ctest ran in the `sonora-build` image; the micro
build runs the targets these files reach and their dependants, which includes `mix_planner`):

    12/34 Test #713: space_measure ....................   Passed    0.24 sec
    13/34 Test #712: mix_planner ......................   Passed    0.35 sec
    ...
    100% tests passed, 0 tests failed out of 34
    The following tests did not run:
        717 - engine_fake_record (Skipped)
    micro-build: ok build (256 s)
    micro-build: all passed

Result: `mix_planner` passes (0.35 s). `engine_fake_record` is reported as skipped
by the test itself; the reason was not checked and it is not in this change's path.

Host note: the Windows host has no `cmake` on its Git Bash PATH, so a bare
`ctest -R mix_planner` on the host is not possible; the container run above is the evidence.

## Open follow-ups

1. **Applying processor settings needs inserts on Aux and Master buses.** The planner
   produces sends and processor settings as a plan only. Aux buses hold no inserts yet
   (ADR-0045 §5), so nothing is applied. Owner: the mix-bus work (#561 still open).
2. **Measuring RT60 and pre-delay from a reference recording** is a separate item.
   `ReverbDelayReference` takes the numbers as input; nothing is measured from audio.
3. Listening check on real material for the snapping tables (ADR-0128, unverified
   assumption), to hand to the researcher.

## Unverified

- A standalone `ctest -R mix_planner` on a host build directory (only the container run above).
- Real-session listening check (follow-up 3).
