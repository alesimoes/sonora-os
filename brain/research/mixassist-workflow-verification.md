# Mix-assistant workflow planners: verification on main (#437)

Scope: the four rule-based planners in `engine/control` (`mix_planner.h`) that
ADR-0045 describes, verified against their test as it stands on `main`
(commit `fec6609f`, bundle #437, an ancestor of `origin/main` 254e3219).
This is a verification report. No code was changed.

## Planners checked

| Planner | Header | Proposes | Test coverage (`test_mix_planner.cpp`) |
|---|---|---|---|
| `GainStagingPlanner` | `mix_planner.h` | `SetMix` trims so each track sits at -23 LUFS after its fader, -3 dBTP per-track ceiling, 6 dB master headroom | Analysis figures on synthetic tones (peak, RMS, LUFS, gating, true peak); trims land within 0.5 LU of -23 LUFS; reason names LUFS and true peak; apply then undo restores the session; re-proposal on the result is a no-op |
| `BusStructurePlanner` | `mix_planner.h` | Role guess per track (drums, bass, guitars, keys, vocals) from name, inserts and colour; one aux per role with two or more tracks (`AddBus`), tracks routed (`SetOutput`); an existing bus of that name is reused | Covered by the same apply/preview/undo harness; the bus-with-inserts path is noted as open (see below) |
| `ParallelPlanner` | `mix_planner.h` | "Drums Parallel" aux when kit crest factor is 10 dB or more; "Bass Saturation" aux for bass; post-fader `AddSend` per track and the native processor's settings | Covered by the apply/preview/undo harness; processor settings are data, not inserts, for the reason in `mix_planner.h:125` |
| `ReverbDelayPlanner` | `mix_planner.h` | Space sends from a reference (decay, pre-delay, echo); aux buses hold no inserts yet (`mix_planner.h:163`) | Covered by the same harness; see the finding below on ADR-0045 |

`propose_mix_plan` (`mix_planner.cpp:795`) builds the four and dispatches by
`MixPlanKind`. Every plan is checked for: no plan problems, the preview equals
what accepting leaves, accept advances the revision by one step, and one undo
returns the session exactly (edit, mix tracks, undo depth).

## Command and output

Build and run, on this worktree (`sub/mixassist.workflow/verify`, based on
`origin/main`), through the micro build that the PR gate uses, with the lock
taken by its stages (`tools/build-slot.sh`). The plan it wrote
(`.micro-build/plan.json`) selected only `test_mix_planner` and `ctest -R '^(mix_planner)$'`:

```
tools/micro-build.sh --no-rtsan --files-from <file listing engine/control/tests/test_mix_planner.cpp>

micro-build: ctest -R '^(mix_planner)$'
Test project /build/micro-build
    Start 677: mix_planner
1/1 Test #677: mix_planner ......................   Passed    1.33 sec

100% tests passed, 0 tests failed out of 1

Label Time Summary:
control    =   1.33 sec*proc (1 test)
Total Test time (real) =   1.38 sec
micro-build: passed
micro-build: ok build (223 s)
micro-build: all passed
[exited with code 0]
```

The build compiled `sonora_core`, `sonora_control` (including `mix_planner.cpp`)
and `test_mix_planner` in a fresh build volume (223 s, ccache 141/153 hits).
`SONORA_DSP_QUICK=1` was set by the stage. Host: Windows 11 Enterprise 10.0.26300,
Docker image `sonora-build:trixie`, 16 jobs.

## Mix-bus chain: still open

The chain (a planner suggestion becoming a working bus with inserts) is NOT
done on `main`. The planners keep the processor as a suggestion, not a step:

- `mix_planner.cpp:558` and `:714`: "put <processor> on the bus; aux buses
  cannot hold inserts yet". The bus-processor notes are emitted and the
  processor goes to `suggestions_`, not into the plan's steps.
- `mix_planner.h:125` and `:163`: aux buses cannot hold inserts yet
  (ADR-0045 §5).
- `brain/decisions/ADR-0045-mix-assistant-workflow-planners.md:62`: the
  remaining work is the BACKLOG item "Aux and Master inserts"; `AddInsert`
  takes a track today, not an aux or the master.

So the chain remains open and waits on Aux and Master inserts. Once those land,
the planners' `suggestions_` can become `AddInsert` steps on the bus, and the
test can assert the step, not a note.

Not checked here: whether master inserts exist in the model. The notes above
cover aux buses; this report did not read `common/model` for a master-insert
path, so that half of the claim rests on ADR-0045 §5 and the BACKLOG line it
names.

## Findings

1. ADR-0045's Summary names three planners (gain, bus, parallel). The code
   and `propose_mix_plan` also carry `ReverbDelayPlanner`, which the ADR does
   not describe in its Summary. Follow-up: the ADR should record the fourth
   planner or say where it is decided.
2. `ADR-0105` was not found in this worktree, in `origin/main` or in the
   `feat/mixassist.plans` remote branch. The assignment said it was already
   written; this report does not cite it. Its INDEX line is therefore not
   given in the PR description.
3. `feat/mixassist.workflow` does not exist locally or on `origin`. The work
   starts from `origin/main` on `sub/mixassist.workflow/verify`.

## Unverified

- Behaviour on real session audio: the test uses synthetic tones and bursts.
- The planners' CPU cost: not measured (planning runs off the audio thread).
- Clang and MSVC: the run used GCC in the Linux container only.
