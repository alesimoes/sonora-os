# Plan: drivers.sync.clock - Word clock and ADAT clock source

Status: approved
Written by: architect   Dispatched by: lead   Architect sign-off: architect 2026-11-09
Machine plan: `.harness/plan/drivers.sync.clock.plan.json`. Grouped items: only
`drivers.sync.clock` (the sole clock item; LTC and MTC are other transports and other features).

## 1. Goal and user experience  (architect)
A machine can choose the interface clock source (internal, ADAT, S/PDIF, word clock);
if an external source is chosen and not locked, recording is refused with a reason.
Out of scope: settings UI, real libasound mixer adapter, hardware validation.
Acceptance: `ctest -R clock` passes.

## 2. Design  (visual-designer)
N/A - no on-screen change.

## 3. Contracts and integrations  (architect + owner engineer)
New module `drivers/clock`: `clock_source.h` (ClockSource, ClockLock, ClockCaps,
ClockSourceControl), `clock_mixer.h` (ClockMixer seam, AlsaClockControl),
`clock_guard.h` (ClockGuard), header-only `testing/fake_clock.h`. ADR-0125.
Reserved: ADR 125, protocol none, format none. CMake `drivers/clock/CMakeLists.txt`
globs `src/*.cpp` into `sonora_clock` and makes one test per `tests/test_*.cpp`
(name `clock_<stem>`, label drivers); hooked by one line in `drivers/alsa/CMakeLists.txt`.
Root CMake and `tools/module-deps.allow` (`drivers/clock -> common/base`) are the
lead's edit after time.groove lands.

## 4. DAW core  (rt-engineer)
N/A - control thread only; nothing runs on the audio thread.

## 5. Plugins, DSP, host
N/A.

## 6. UI
N/A.

## 7. Tests  (tester)
Contract: `test_clock_contract.cpp` (fakes). Tests slice: `test_clock_guard.cpp`
(refuse unavailable, refuse unlocked, Unknown needs confirmation, lock loss counted,
transport untouched, bounded polls) and `test_alsa_clock_control.cpp` (element-name
table against FakeClockMixer). No wall clock.

## 8. QA
Unit only. Card behaviour unverified (ADR-0125 Unverified list -> researcher).

## 9. Quality checks
No silent fallback; never stops transport. Files under ~500 lines.

## 10. Code standards
`.claude/rules/code-structure.md`, `testing-and-flakes.md`, `gates-and-checks.md`.

## 11. Dependencies and order
contract -> impl -> tests, one feature branch, one PR.
| Slice | Agent | Difficulty | Risk | Tier | Sub-branch | Files |
|---|---|---|---|---|---|---|
| contract | driver-engineer | M | high | opus | feat/drivers.sync.clock-contract | headers, fake, CMake, ADR, contract test |
| impl | driver-engineer | M | high | opus | feat/drivers.sync.clock-impl | clock_guard.cpp, alsa_clock_control.cpp |
| tests | driver-engineer | M | med | sonnet | feat/drivers.sync.clock-tests | guard and alsa control tests |
Integrator: lead. PR title: Sync: external clock source (ADR-0125) (partial: not validated on hardware).

## 12. Risks and open questions
| Question | Recommended default |
|---|---|
| UMC1820 exposes selector/validity? (researcher) | assume unknown; Unknown-lock path with confirmation |
| Word clock input exists? | assume ADAT/S-PDIF only; keep the enum value |
| Missing control-tick hook | report to architect, do not add |

## 13. Done =
- [ ] contract slice merged
- [ ] slices merged by the integrator
- [ ] section 7 tests pass (commands and output in the PR)
- [ ] `tools/pre-pr.sh` clean
