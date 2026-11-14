# Plan: ui.editing.import - Import audio with sample-rate conversion

Status: approved

Architect sign-off: ADR-0174 decided, no protocol or format change. Items grouped: ui.editing.import only (single item; the feature is on main, the PR records the decision and closes two test gaps).

## 1. Goal and user experience
Import dialog converts files to the session rate, places them as new tracks in one undo step. Already works on main.

## 2. Design
N/A: no UI change.

## 3. Contracts and integrations
ADR-0174 (decisions 1-5). No contract change.

## 4. DAW core
N/A: UI-side code, not the RT path.

## 5. Plugins, DSP, host
N/A.

## 6. UI
No change to `audio_importer.cpp`. If a decision disagrees with the code, stop and report.

## 7. Tests
`ui/qt/tests/test_import.cpp`: (a) model refusal after rename deletes the files and places nothing (FakeEngine track-limit); (b) cancel mid-conversion asserts Cancelled, no files left. Wait on progress, never on the clock.

## 8. QA
tester runs `tools/micro-build.sh` for ui_import and audio_import.

## 9. Quality checks
`tools/pre-pr.sh`; micro build.

## 10. Code standards
Split test_import.cpp if past ~500 lines (345 now).

## 11. Dependencies and order
Tasks: adr (junior, S/low), tests (pleno, S/low). One PR.

## 12. Risks and open questions
None; default: if a decision disagrees with code, the architect amends the ADR.

## 13. Done =
ADR and INDEX line on main, both tests green, PR carries the [x] line.
