# Plan: time.elastic - Elastic Audio: warp markers, algorithms, Quantize Audio

Status: approved
Written by: architect   Dispatched by: lead   Architect sign-off: architect 2026-11-09
Grouped items: only `time.elastic` is open in this area. Live (non-destructive) warping and
persisted markers are a later item, see ADR-0136 section 1.
Machine plan: `.harness/plan/time.elastic.plan.json`. ADR-0136 reserved. No protocol, no format.

## 1. Goal and user experience  (architect)
Select a region, Tempo > Elastic: set a ratio, or place warp markers (source position -> target
position), or Quantize Audio (onsets move to a beat grid at a strength). Pick an algorithm
(Polyphonic, Rhythmic, Monophonic, Varispeed). Apply renders a new file and the region plays
it; Undo restores the original in one step. A refusal (overlap with the next region, no onsets,
engine without the library) is a held notice.
Out of scope: live warping, persisted markers, pitch shifting, tempo-follow of a whole track.

## 2. Design  (visual-designer)
N/A - one panel in the existing editor style, own names, no brands.

## 3. Contracts and integrations  (architect + owner engineer)
`contracts/api/include/sonora/api/elastic.h`, command `audio.elastic`, capability `audio.elastic`,
fake engine. Protocol unchanged, session format unchanged. ADR-0136.
Acceptance: `ctest -L contract` incl. the elastic test.

## 4. DAW core  (rt-engineer)
`engine/elastic`: control-thread job, Rubber Band adapter (GPL-2+, optional via pkg-config),
exclusive-create synced file, one Compound. Nothing on the audio thread.
Acceptance: fake-stretcher tests (one undo step, original untouched, overlap refused, cancel
cleans up, capability absent without the library).

## 5. Plugins, DSP, host  (dsp-engineer)
`common/stretch`: pure WarpMap, quantize planner, Stretcher interface + fake. The algorithm
itself is Rubber Band, never a proprietary one.

## 6. UI  (ui-engineer)
`ui/elastic` (own module), `elastic_command`, `WarpPanel.qml`, one-line hook in edit_tempo.cpp.
Do NOT edit EditWindow.qml, command.h, edit_controller.h, ui/qt/CMakeLists.txt; stop and message
the architect if unavoidable. Screenshots `docs/images/elastic-*.png`.

## 7. Tests  (tester)
One file per area; failure cases must exist (overlap, bad markers, no onsets, no library, cancel).

## 8. QA
Screenshots opened. Audio quality unverified (ADR Unverified 2).

## 9. Quality checks
No RT path touched. Review focus: original take never written, partial file removed, one undo
step, capability honest.

## 10. Code standards
`.claude/rules/code-structure.md`, `testing-and-flakes.md`, `gates-and-checks.md`.

## 11. Dependencies and order
| Slice | Agent | Difficulty | Risk | Tier | Sub-branch | Starts from | Files |
|---|---|---|---|---|---|---|---|
| contract | rt-engineer | M | high | senior | feat/time.elastic-contract | main | api/elastic.h, command.h entry, fake, ADR |
| stretch | dsp-engineer | M | med | pleno | feat/time.elastic-stretch | main | common/stretch, root CMakeLists lines |
| engine | rt-engineer | L | high | senior | feat/time.elastic-engine | contract+stretch | engine/elastic, one hook line |
| ui | ui-engineer | L | med | senior | feat/time.elastic-ui | contract+stretch | ui/elastic, scene, hook |

Order: contract and stretch in parallel, then engine and ui in parallel. One PR from
`feat/time.elastic`. Shared build files are avoided: the root CMakeLists lines for
`engine/elastic` and `ui/elastic` (guarded by `if(EXISTS)`) are added by the stretch task;
`engine/control/CMakeLists.txt` (ui.mixer.sidechain) is not edited. command.h gets one catalog
entry in the contract task; if ui.workflow.windows has it too, the lead sequences them.

## 12. Risks and open questions
| Question | Recommended default |
|---|---|
| Result longer than the gap | Refuse with a reason, no ripple |
| Library missing in the image | Capability off, loud notice; researcher confirms the package (ADR Unverified 1) |
| Onset source | Injected; beat detector if merged, else the peak picker |

## 13. Done =
- [ ] tests of sections 3-6 pass (commands and output in the PR)
- [ ] `tools/pre-pr.sh` clean
- [ ] screenshots opened and listed
