# Plan: drivers.sync.ltc - LTC chase and generate

Status: approved
Written by: architect   Dispatched by: lead   Architect sign-off: architect 2026-11-09
Machine plan: `.harness/plan/drivers.sync.ltc.plan.json`. Grouped items: only
`drivers.sync.ltc`; MTC, MIDI clock and the clock item stay separate
(different transport, other active feature).

## 1. Goal and user experience  (architect)
A session can follow incoming LTC or emit LTC from its transport, at codec and
driver level. Out of scope: the audio-thread channel tap, settings UI,
hardware validation.
Acceptance: `ctest -R ltc` passes.

## 2. Design  (visual-designer)
N/A - no on-screen change.

## 3. Contracts and integrations  (architect + owner engineer)
New module `drivers/ltc`: `ltc.h` (LtcTime with inline frame/sample maths,
LtcDecoder, LtcEncoder), `ltc_sync.h` (LtcChase, LtcChaseDriver, LtcGenerator,
LtcSampleSource, LtcSampleSink), header-only fakes. ADR-0124. Reserved: ADR 124,
protocol none, format none.
CMake: `drivers/ltc/CMakeLists.txt` globs `src/*.cpp` (CONFIGURE_DEPENDS) into
`sonora_ltc` and makes one test target `ltc_<name>` per `tests/test_*.cpp`, so
later slices add files with no CMake edit. The contract slice owns a trivial
`src/ltc_version.cpp` so the library is never empty.
Root `CMakeLists.txt` is claimed by time.groove, so the module is hooked from
`drivers/support/CMakeLists.txt` with `add_subdirectory(../ltc ltc)` and sets
its own test labels. The lead moves it to the root list (plus
`sonora_label_module_tests(drivers/ltc drivers)`) once time.groove has landed,
and adds `drivers/ltc -> contracts/api` to `tools/module-deps.allow`.

## 4. DAW core  (rt-engineer)
N/A - control thread only; the codec is allocation-free so the later
audio-thread wiring is possible.

## 5. Plugins, DSP, host
N/A.

## 6. UI
N/A.

## 7. Tests  (tester)
Contract slice: `test_ltc_contract.cpp` (frame/sample maths, drop-frame vectors,
fakes behave). Tests slice: `test_ltc_codec.cpp` (synthetic round trip at 4
rates and 44.1/48/96 kHz, block-size independence, garbage yields nothing) and
`test_ltc_sync_behaviour.cpp`. Tick-based, no wall clock.

## 8. QA
Unit only. Real signal quality is unverified (ADR-0124).

## 9. Quality checks
Chase never locates while recording. Codec allocation-free (rt-safety-auditor
reviews). Files under ~500 lines.

## 10. Code standards
`.claude/rules/code-structure.md`, `testing-and-flakes.md`, `gates-and-checks.md`.

## 11. Dependencies and order
contract -> impl -> tests, one feature branch, one PR.
| Slice | Agent | Difficulty | Risk | Tier | Sub-branch | Files |
|---|---|---|---|---|---|---|
| contract | driver-engineer | M | high | opus | feat/drivers.sync.ltc-contract | headers, fakes, ADR, CMake, version stub, contract test |
| impl | driver-engineer | M | high | opus | feat/drivers.sync.ltc-impl | ltc_codec.cpp, ltc_driver.cpp |
| tests | driver-engineer | M | med | sonnet | feat/drivers.sync.ltc-tests | test_ltc_codec.cpp, test_ltc_sync_behaviour.cpp |
Integrator: lead. PR title: Sync: LTC chase and generate (ADR-0124) (partial: no audio tap, no hardware).

## 12. Risks and open questions
| Question | Recommended default |
|---|---|
| Signal level/jitter on UMC1820 (researcher) | adaptive threshold; measure |
| Varispeed lock range | assume +-10 %, measure |
| Unify with MTC Timecode | later refactor |
| Missing transport command id | report to architect, do not add |

## 13. Done =
- [ ] contract slice merged
- [ ] slices merged by the integrator
- [ ] section 7 tests pass (commands and output in the PR)
- [ ] `tools/pre-pr.sh` clean
