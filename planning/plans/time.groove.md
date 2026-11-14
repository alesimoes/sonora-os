# Plan: time.groove - Groove track and groove templates

Status: approved
Written by: architect   Dispatched by: lead   Architect sign-off: architect 2026-11-08
Grouped items: only `time.groove` is open in this area; Beat Detective slicing, MIDI grooves
and a beat-detector-based extractor are follow-ups.

## 1. Goal and user experience  (architect)
The user picks a groove (a swing preset, or "from selected region" which reads a reference region's
timing), sets a strength, and runs Tempo > Groove (`tempo.edit groove ...`) on the selected regions or
range. Regions move onto the groove's slots in one step; Undo reverts. A message says how many were moved
and how many skipped (would overlap a neighbour). Saved templates are listed in a small panel.
Out of scope: hit-level timing inside a region, MIDI, real-time groove.
Acceptance: the model and ui tests below pass.

## 2. Design  (visual-designer)
N/A - one small panel in the existing editor style; own names, no brands.

## 3. Contracts and integrations  (architect + owner engineer)
ADR-0115. `common/groove/include/sonora/groove/groove.h` is the contract (pure value and functions). Protocol none, formats
none, no command.h change (`tempo.edit` gains the word `groove`). Reserved: ADR 115.
Acceptance: `ctest -L model` incl. `model_groove`; protocol constant unchanged.

## 4. DAW core  (rt-engineer)
`groove.h/.cpp` per ADR-0115 Decision 1-2. Off the audio thread; no new Operation.
Acceptance: `test_groove`: swing values, extraction (even, swung, sparse), strength 0/50/100,
overlap skip, refusals, MusicalMap round trip.

## 5. Plugins, DSP, host  (dsp-engineer)
N/A - no DSP; the onset picker is a peak-envelope threshold in the ui.

## 6. UI  (ui-engineer)
`edit_groove.cpp/.h` (new files), `OnsetSource` interface + peak-based implementation + fake,
`GroovePanel.qml`, preferences JSON load/save, one-line hook for `groove` in edit_tempo.cpp.
Do NOT edit EditWindow.qml or command.h (ui.workflow.windows); stop and message the architect if needed.
Acceptance: `test_ui_groove`, screenshots `docs/images/groove-*.png`.

## 7. Tests  (tester)
One file per area; tester checks failure cases exist (no reference, overlap skip, strength 0, bad file).

## 8. QA
Screenshots opened. Real loops unverified (see ADR Unverified).

## 9. Quality checks
No RT path touched. Review focus: no overwrite of neighbours, one undo step, skipped count shown.

## 10. Code standards
`.claude/rules/code-structure.md`, `testing-and-flakes.md`, `gates-and-checks.md`.

## 11. Dependencies and order
| Slice | Agent | Difficulty | Risk | Tier | Sub-branch | Starts from | Files |
|---|---|---|---|---|---|---|---|
| model | rt-engineer | M | med | pleno | feat/time.groove-model | main | common/groove (own module), root CMakeLists line, ADR-0115 |
| ui | ui-engineer | L | med | senior | feat/time.groove-ui | model | ui/groove (own module) + scene + one-line hook |

Order: model, ui. One PR from `feat/time.groove`.
Active feature time.arrangement claims `common/model/CMakeLists.txt`, `ui/qt/CMakeLists.txt`,
`edit_controller.h`, so this feature touches none of them: the model part is the new module `common/groove`
(own CMakeLists, `add_subdirectory` line in the root CMakeLists); the ui part is the new module `ui/groove`
(own CMakeLists adding its sources to `sonora_ui_session` with `target_sources`/`target_link_libraries`,
absolute paths, and its test target), a scene in `ui/qt/src/scenes/groove.cpp` (picked up by the existing
GLOB), and a one-line hook in `ui/qt/src/edit_tempo.cpp`. The entry point is a free function
`bool groove_command(EditController&, const std::string& text, std::string& why)` using only the public
EditController API; no `edit_controller.h` change. If the panel cannot be wired without editing
`EditWindow.qml`, `command.h`, `edit_controller.h` or `ui/qt/CMakeLists.txt`, stop and message the architect.

## 12. Risks and open questions
| Question | Recommended default |
|---|---|
| Whole-region moves musically thin | Accept; slice-then-groove is a follow-up |
| Templates outside the session | Preferences JSON; revisit if owners want them in sessions |

## 13. Done =
- [ ] tests of sections 3-6 pass (commands and output in the PR)
- [ ] `tools/pre-pr.sh` clean
- [ ] screenshots opened and listed
