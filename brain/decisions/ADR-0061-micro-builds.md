# ADR-0061: Micro builds as the per-PR gate, the full build once a day on main

Status: Accepted (owner, 2026-11-07: "precisamos ter micro builds, certificar que conseguimos buildar o código que foi escrito sem precisar rodar o build de tudo"; one full build a day on main).
Builds on: ADR-0007 §1 (modules, one include directory each, the dependency graph), ADR-0047 (the orchestrator and its gate).
Supersedes: the local gate of 2026-11-05 (`tools/local-ci.sh`, `harness/localci.py`: batches, bisection, the `local-ci` commit status) and the "tests by context" rule of the same day (a PR paid its context: a whole tree build, then TSan, RTSan, record, BWF). Both are removed by this change.

## Summary

A PR's gate is a **micro build**: from the paths it changes, the CMake targets that own them and every target that depends on those are built, and only the tests that run one of them are executed; nothing affected means nothing built. The gate runs on GitHub's free runners (`ci.yml` jobs "Micro build" and "Micro build: RTSan"), and the orchestrator merges on the PR's GitHub checks alone; no local run gates a PR any more (owner, 2026-11-07: `tools/local-ci.sh` retired as a gate). Linking the application, the sanitizers (TSan, ASan), the ISO, the record, BWF and quantum interop suites, the Windows build and the slow tests run **once a day on main** as the GitHub workflow `nightly.yml`, and a red run there opens a fix task of its own (`harness/nightly.py`). A PR touching the real-time call graph still runs RTSan, on the affected targets only. PRs always have base `main`; a stacked PR is retargeted.

## Context

The machine-wide lock (`tools/build-slot.sh`) serialises every heavy run. Each PR gate was a full tree build with every context's steps (the tree step alone took 77 minutes on 2026-11-07, then TSan, ASan, RTSan, record, BWF, Windows), batches of PRs failed on one member and were bisected, and PRs waited hours for a verdict that, for most of them, only had to say "what you wrote compiles and its tests pass". A stacked PR (base = another PR) waited for its base and held everything queued behind it.

## Decision

1. **The unit is the CMake target.** `tools/micro_targets.py` reads the CMake file API (codemodel v2) of a configured tree and `ctest --show-only=json-v1`:
   - a changed file owned by a target's source list affects that target; a header, a `CMakeLists.txt` or any other file under a target's source directory affects every target of that directory;
   - dependents are followed **transitively** (a header of `common/base` changes what `engine/core` compiles against, so the gate must compile `engine/core`); `--direct` exists for a cheaper, less safe run;
   - a test runs when any word of its ctest command is an artifact of an affected target (the engine-process tests name `sonora-engine` by path);
   - **libraries and test executables only** (owner, 2026-11-07: a PR gate checks that the code works; producing the product is a release process). An application executable (`sonora-ui`, `sonora-analyze`, `sonora-plugin-host`, `sonora-engine`) is linked only when a selected test runs it; otherwise the daily full run and the release link it;
   - a root build file (`CMakeLists.txt` at the root, `cmake/`, `tools/*.cmake`) means the whole tree; a path no target owns (docs, the harness, the dashboard) means nothing.
2. **One script, two stages.** `tools/micro-build.sh` computes the paths from the merge base with `origin/main`, configures once in the build image (`sonora-build`, or `sonora-ui` when `ui/` is touched, so Qt is there), builds the plan's targets and runs `ctest -R` on its tests (`SONORA_DSP_QUICK=1`, never the `slow` label), then the two engine-process integration scripts (`tools/failed-take-quit.py`, `tools/crash-recovery-kill.py`) when `sonora-engine` was among the targets. When the change is in the RT call graph (`engine/`, `drivers/`, `common/` but `dsp`, `plugins/native`, `plugins/client`, `plugins/sdk`, `contracts/`), a second stage runs `tools/build-rtsan.sh` on the same targets (`SONORA_RTSAN_TARGETS`) with the same `-R`. The same script runs on a developer's machine through the lock (`tools/build-slot.sh`).
3. **GitHub runs it.** `ci.yml` gets `micro` (the Release stage, ccache restored between runs) and `micro-rtsan` (the RTSan stage, when `changes.rt` is true), both in the GHCR images `images.yml` publishes. A docs or harness PR skips both (a skipped job passes).
4. **The daemon merges on the GitHub checks.** `train._step` merges a PR whose checks are all green and whose head is mergeable; the micro build is one of them. `harness/localci.py`, `localci_cancel.py`, `tools/local-ci.sh`, the `local-ci` commit status, the batch, the bisection and the pre-PR local run are deleted; `Config.local_ci` and `local_ci_slots` with them. The dashboard's builder card reads nothing when `status.json` carries no `builder`.
5. **Main, once a day, on GitHub.** `.github/workflows/nightly.yml` (cron 03:00 UTC, `workflow_dispatch`) carries the jobs `ci.yml` ran per PR until 2026-11-05 and `main-full.yml` ran per merge, lifted as they were: engine build and every test, MSVC portable, Qt UI, TSan and ASan, RTSan over the whole tree, record, BWF interop, quantum, plugin host, the slow DSP suites in full, the CPU figures, the ISO with the USB writer. `harness/nightly.py` polls its newest completed run every `Config.nightly_poll_s`; a failed run not seen before opens a work task `main-red-<sha8>` for the track's engineer with the failure excerpt as brief; no PR is blamed and no PR waits for it.
6. **No stacked PRs.** The train adopts a PR whose base is another PR and retargets it to `main` at once (before: it waited for the base). CLAUDE.md states the rule: a PR always has base `main`; work that depends on an unmerged PR waits or ships in the same PR.

## Rationale

What a PR must prove is that its code compiles and its tests pass; the sanitizers and interop suites prove properties of the whole tree that one PR rarely breaks and that a daily run catches within a day, with a fix task instead of a blocked merge. The target graph is the one source of "what depends on what" that is always current (the CMake tree), so no hand-kept table of contexts goes stale. Transitive dependents keep the gate honest at the price of a shared-header change building most of the tree; that price is the module boundary problem, listed below, not a reason to skip dependents. GitHub's runners are slow but free and parallel: a micro build of one module fits them, where the full tree never did, and the machine's lock is left to main's daily run and the fallback.

## Module boundaries not yet separable (follow-ups)

Transitive dependents of these build most of the tree; a change there is a near-full build until the boundary is split. Each is a BACKLOG follow-up, not part of this change:

- `common/base` (every module links it): its headers are the shared vocabulary; a split by concern (`ring_buffer`, `frames`, `logging`) would let a change touch one slice's dependents.
- `common/platform` (the OS seam, ADR-0008): linked by `common/audiofile`, `common/session_files`, `engine/plugin_client`, the drivers; a header change rebuilds the engine and the UI client.
- `contracts/api`, `contracts/backend`, `contracts/plugin`, `contracts/surfaces`: the wire and ABI headers; any edit rebuilds both sides by design (a contract change is an amendment, CLAUDE.md), so the full dependents are the right scope, but their tests should be split from the conformance suite so a header comment does not run it.
- `common/model` (ADR-0059): the engine, the UI and `plugins/analyze` depend on it; a family-level target split (one target per `apply_<family>.cpp` group) would narrow a change to its family.
- `engine/core`: one target for the whole RT core; its tests (`core_*`) run as one block. Splitting `transport`, `record`, `mix` into targets is the step that makes an engine PR's micro build small.
- Tests whose command is a script running a built binary by a path not in the target's artifacts (`tools/failed-take-quit.py`, `tools/crash-recovery-kill.py`) are matched by basename; a renamed binary would silently drop them from the plan. A `--check` that lists tests no target reaches is a follow-up.
- The nightly's jobs are the 2026-11-05 ones unchanged; scripts they call have moved on since (`tools/build-plugins.sh --plugins-only`, the ISO pre-flight). The first scheduled runs will show what drifted; each red is a fix task by design.

## Consequences

- A PR's gate time is the configure plus the affected targets: minutes for a plugin, a UI module or a driver; tens of minutes for `engine/core`; the whole tree only for a root build file or `common/base`. Before: 77 min for the tree step plus the sanitizers, per batch, serialised on one machine.
- The machine's lock is freed for developers' own micro builds and the ISO; PR batches, bisects and the "batch red, who did it" hour are gone with the code that ran them.
- A red nightly is a task on the board, visible, assigned; it does not stop merges. A defect that the micro build does not see (a TSan race, a BWF reader regression) may reach main and be fixed the next day; that is the trade the owner asked for.
- The `changes` filter of `ci.yml` remains for the `rt` and `ui` flags and the image rebuild flags; the os-image tests (`tools/test-*-qemu.sh`, KVM) stay on the owner's machine, run by hand or by the release.
