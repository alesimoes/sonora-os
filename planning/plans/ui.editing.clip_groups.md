# Plan: ui.editing.clip_groups - Clip groups and clip looping; region loops and folders

Status: approved
Written by: architect   Dispatched by: lead   Architect sign-off: architect 2026-11-08
Machine-readable twin: `.harness/plan/ui.editing.clip_groups.plan.json`.

## 1. Goal and user experience  (architect)
Most of the item is on main since ADR-0052 (#438): Group Clips, Ungroup, Loop
Clip, Unloop, group move as one, tags on the region, session format 24,
screenshot `sonora-ui-edit-clip-groups.png`, tests in `common/model` and
`ui/qt`. What is left, grouped into this one feature and PR: (1) a
`Loop Clip...` menu entry that asks for the number of repeats in a dialog
offering the last count (Pro Tools asks; our key `ctrl+alt+l` keeps looping
silently with the last count); (2) ADR-0098 recording what is deferred and why.
"Folders" in the BACKLOG title is `ui.mixer.folders` (own plan), not this
feature. Out of scope: MIDI, nested groups.
Acceptance: `clip.loop` with a typed count lays that many repeats as one
"Loop Clip" step; `edit.repeat` is unchanged.

Open parts NOT in this PR (BACKLOG lines in the PR body):
- loop handle dragged on a clip's right edge (Logic: Loop regions): needs
  `edit_gestures.cpp`, `edit_controller.h`, `timeline_scene.cpp`, held by
  `ui.editing.sample_edit`; plan it after that merges.
- trim of a member trimming its repeats: ADR-0052 chose copies; owner decision,
  default no.
- Automation Follows Edit following a group move.
So the PR is partial and its title says so; the coordinator must not tick the
BACKLOG line `[x]` for the whole item.

## 2. Design  (visual-designer)
N/A - the existing Repeat dialog, parametrised; screenshot
`docs/images/sonora-ui-edit-loop-dialog.png`.

## 3. Contracts and integrations  (architect + owner engineer)
No protocol, no format, no model change. ADR-0098 (reserved) is the only
contract. `Main.qml` is held by `plugins.offline`: this feature declares `depends_on_feature: plugins.offline` and the lead starts it only after that PR merges (the dialog cannot be done without Main.qml; the QML file list is in held `ui/qt/CMakeLists.txt`, so no new QML file either). Held by other features, so untouched: `ui/qt/CMakeLists.txt`,
`ui/qt/tests/CMakeLists.txt`, `session_model.h`, `shell_model.cpp`,
`edit_controller.h`, `edit_gestures.cpp`, `edit_clip_groups.cpp`,
`EditWindow.qml`, `timeline_scene.*`, `brain/INDEX.md` (the INDEX line goes
in the PR body).
Reserved: ADR 98, protocol none, format none.

## 4. DAW core  (rt-engineer)
N/A - nothing on the audio thread; repeats are plain regions (ADR-0052).

## 5. Plugins, DSP, host
N/A - none.

## 6. UI  (ui-engineer)
Task `dialog` in the JSON: Repeat dialog in `Main.qml` made generic over the
command, menu entry, scene, test.

## 7. Tests  (tester)
In `ui/qt/tests/test_clipboard.cpp`: `clip.loop` with "4", with a bad
argument (refused with notice), with none (last count). Existing `edit.repeat`
tests must still pass. Label `ui`.

## 8. QA  (tester)
Open the dialog from the menu, type a count, undo once removes all repeats;
Repeat dialog still works. Screenshot `sonora-ui-edit-loop-dialog.png`.

## 9. Quality checks
No RT code. `Main.qml` is over 500 lines already: no growth beyond the dialog
change; a split is a separate refactor task.

## 10. Code standards
`.claude/rules/code-structure.md`, `testing-and-flakes.md`, `gates-and-checks.md`.

## 11. Dependencies and order
contract -> dialog -> one PR.
| Slice | Agent | Difficulty | Risk | Tier | Sub-branch | Starts from | Files |
|---|---|---|---|---|---|---|---|
| contract | ui-engineer | S | low | junior | feat/ui.editing.clip_groups-contract | main | ADR-0098 |
| dialog | ui-engineer | M | med | pleno | ...-dialog | contract | Main.qml, scenes/edit_loop_dialog.cpp, test_clipboard.cpp, 1 PNG |

Integrator: lead names one.
Single PR title: ui.editing.clip_groups: Loop Clip... dialog and ADR-0098 (loop handle drag waits on sample_edit)

## 12. Risks and open questions
| Question | Recommended default |
|---|---|
| Does the new scene register without a held CMake file? Unverified; check `edit_repeat.cpp` | if not, stop and ask the architect |
| Should the shortcut ask too (Pro Tools does)? | no; silent with the last count, the menu asks |
| Trim of a member trims its repeats | no, repeats stay independent copies |

## 13. Done =
- [ ] contract merged into the feature branch
- [ ] dialog slice merged by the integrator
- [ ] section 7 tests pass (commands and output in the PR)
- [ ] screenshot committed and opened
- [ ] `tools/pre-pr.sh` clean
