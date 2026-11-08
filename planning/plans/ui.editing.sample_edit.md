# Plan: ui.editing.sample_edit - Sample-level editing: pencil repair, audio file editor

Status: approved
Written by: architect   Dispatched by: lead   Architect sign-off: architect 2026-11-08
Machine-readable twin: `.harness/plan/ui.editing.sample_edit.plan.json`.

## 1. Goal and user experience  (architect)
Most of the item is on main: ADR-0052 §4 delivered the engine's `sample.edit`
(repair, draw, silence, reverse, invert, gain, normalize, fades), the `sample.*`
commands and `EditController::sample_edit_argument`. Open items grouped here
because they are one gesture and one panel in one window: (1) a Pencil drawn
with the pointer on the waveform; (2) a view of individual samples; (3) a
Sample Edit panel for the other verbs. Flow: zoom in to 4 frames per pixel or
less, the overview turns into the sample curve; with the Pencil, drag across a
clip; on release the engine writes a copy with the drawn samples (one undo
step, take untouched). With the Selector, select a range in one clip and press a
panel button. Out of scope: MIDI, spectral editing, a separate window.
Acceptance: a Pencil stroke at sample zoom sends exactly one `sample.edit draw`.

## 2. Design  (visual-designer)
N/A - a curve and a button panel in the existing Edit window; the ui-engineer
follows the existing look. Screenshots listed in section 8.

## 3. Contracts and integrations  (architect + owner engineer)
No protocol, no format. ADR-0096 (reserved). New UI-side interface
`SampleSource` (`ui/qt/src/sample_view.h`, header-only) with a fake and a test.
Constraint: `ui/qt/CMakeLists.txt`, `ui/qt/tests/CMakeLists.txt`,
`session_model.h`, `shell_model.cpp` are held by active features, so no new
compiled or QML file: header-only code, existing .cpp files, globbed scenes.
Reserved: ADR 96, protocol none, format none.
Acceptance: contract test green via `tools/micro-build.sh`.

## 4. DAW core  (rt-engineer)
N/A - the engine side exists (ADR-0052). Nothing on the audio thread: the WAV
window is read on a worker thread of the interface.

## 5. Plugins, DSP, host
N/A - none.

## 6. UI  (ui-engineer)
Tasks `core` and `view` in the JSON: gesture and window source, then drawing and
panel. Bound to `SampleSource` and the existing `sample.edit` command.

## 7. Tests  (tester)
Written by each slice in existing test files (no CMake edit possible):
`test_clip_groups.cpp` (contract), `test_edit.cpp` (stroke, one send, coarse
refusal, clip edge, recording), `test_render.cpp` (held notice). Label `ui`.

## 8. QA  (tester)
Stroke at sample zoom and undo; panel verbs on a range; refusal while
recording. Screenshots: `docs/images/sonora-ui-edit-sample-view.png`,
`sonora-ui-edit-sample-panel.png`.

## 9. Quality checks
No RT code. Review focus: the worker thread is never touched from paint; the GUI
answers from memory. `timeline_scene.cpp` (823) and `EditWindow.qml` (658) are
already over ~500; a follow-up extracts the panel and window source once the
CMake files are free (BACKLOG line from the integrator).

## 10. Code standards
`.claude/rules/code-structure.md`, `testing-and-flakes.md`, `gates-and-checks.md`.

## 11. Dependencies and order
contract -> core -> view -> one PR.
| Slice | Agent | Difficulty | Risk | Tier | Sub-branch | Starts from | Files |
|---|---|---|---|---|---|---|---|
| contract | architect | M | high | opus | feat/ui.editing.sample_edit-contract | main | ADR-0096, INDEX, sample_view.h, test_clip_groups.cpp |
| core | ui-engineer | L | med | opus | ...-core | contract | sample_window.h, edit_controller.h, edit_gestures.cpp, edit_clip_groups.cpp, test_edit.cpp |
| view | ui-engineer | L | med | opus | ...-view | core | EditWindow.qml, scenes/edit_sample_*.cpp, timeline_scene.*, test_render.cpp, 2 PNGs |

Integrator: lead names one.
Single PR title: ui.editing.sample_edit: Pencil on the sample-level waveform and Sample Edit panel (ADR-0096)

## 12. Risks and open questions
| Question | Recommended default |
|---|---|
| Does reading a WAV window of a 16M-frame clip stay quick from a USB stick? Unverified; researcher measures frames/s and memory | load only the viewport plus one screen either side |
| Zoom threshold for the sample view | 4 frames per pixel |
| Separate Audio File Editor window (Logic) | not now; the panel in the Edit window covers it |

## 13. Done =
- [ ] contract merged into the feature branch
- [ ] slices merged by the integrator
- [ ] section 7 tests pass (commands and output in the PR)
- [ ] screenshots committed and opened
- [ ] `tools/pre-pr.sh` clean
