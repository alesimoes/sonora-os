# Plan: time.arrangement - Arrangement sections: move and copy

Status: approved
Written by: architect   Dispatched by: lead   Architect sign-off: architect 2026-11-08
Grouped items: only `time.arrangement` is open in this area (time.groove, time.beat_detection,
time.tempo_edit have their own plans). Model and UI ship as one PR.

## 1. Goal and user experience  (architect)
Range markers are sections. In Memory Locations a Sections list offers Move to... and Copy to... with a
destination in the counter format. The section's audio (and the markers in it) goes there, later material
shifts to make room, Move closes the gap it left; one undo step; a message states what was not moved
(automation, tempo, crossfades across the cut). Out of scope: a ruler lane with dragging, a section
`kind`, MIDI, automation following the move.
Acceptance: model and ui tests below pass.

## 2. Design  (visual-designer)
N/A - a list inside the existing Memory Locations window, same style; own names.

## 3. Contracts and integrations  (architect + owner engineer)
ADR-0116. `common/model/arrangement.h` is the contract (pure planner returning a Compound). No new
Operation, protocol none, formats none, no `command.h` change. Reserved: ADR 116.
Acceptance: `ctest -L model` incl. `model_arrangement`; protocol and format constants unchanged.

## 4. DAW core  (rt-engineer)
`arrangement.h/.cpp` per ADR-0116 Decision 2-4. Off the audio thread.
Acceptance: copy, move, straddling regions, markers, destination inside section, 256 cap refusal,
undo round trip, ids strictly increasing.

## 5. Plugins, DSP, host
N/A - no processor.

## 6. UI  (ui-engineer)
`edit_arrangement.cpp/.h` (new), `EditController::arrangement_*`, Sections list in
`MemoryLocationsWindow.qml`. Do NOT edit EditWindow.qml, Main.qml or command.h; stop and message the
architect if needed.
Acceptance: `test_ui_arrangement`, screenshot `docs/images/sonora-ui-edit-arrangement.png`.

## 7. Tests  (tester)
One file per area; check refusals (inside self, over cap, no audio), one undo step, message text.

## 8. QA
Screenshot opened. Real-session Compound sizes unverified (ADR Unverified).

## 9. Quality checks
No RT path touched. Review focus: nothing overwritten, one undo step, cap refusal not silent.

## 10. Code standards
`.claude/rules/code-structure.md`, `testing-and-flakes.md`, `gates-and-checks.md`.

## 11. Dependencies and order
| Slice | Agent | Difficulty | Risk | Tier | Sub-branch | Starts from | Files |
|---|---|---|---|---|---|---|---|
| model | rt-engineer | L | high | senior | feat/time.arrangement-model | main | common/model arrangement, ADR-0116 |
| ui | ui-engineer | M | med | pleno | feat/time.arrangement-ui | model | ui/qt arrangement |

Order: model, ui. One PR from `feat/time.arrangement`. Shared files (`edit_controller.h`,
`ui/qt/CMakeLists.txt`, `common/model/CMakeLists.txt`): start a slice only when no active feature claims them.

## 12. Risks and open questions
| Question | Recommended default |
|---|---|
| Selection markers count as sections | Yes; `kind` is a later format bump |
| Automation follows the section | No in v1; message says so |
| Over the 256 cap | Refuse with the count; chunking is a follow-up |

## 13. Done =
- [ ] tests of sections 3-6 pass (commands and output in the PR)
- [ ] `tools/pre-pr.sh` clean
- [ ] screenshot opened and listed
