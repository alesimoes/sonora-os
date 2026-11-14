# Plan: logic.loop_browser - Loop browser

Status: approved (closed)
Written by: architect   Dispatched by: lead   Architect sign-off: closure, decided by ADR-0163 and ADR-0227

Closure record. The feature is complete on `main`; this plan is kept so the
slices and their decisions can be found from the backlog item.

## 1. Goal and user experience  (architect)
Operator browses loops from a catalog, filters them by name, tag and tempo,
previews one at the session tempo, and inserts it onto a track at a frame
position through the normal edit operations.
Out of scope: anything beyond the catalog, audition and inserter described
in ADR-0163.
Acceptance: the feature is on `main` (PR #686, merge `dd7406fe`).

## 2. Design  (visual-designer)
N/A - the browser panel was drawn in the UI slice of PR #686; its screenshot
is in that PR.

## 3. Contracts and integrations  (architect + owner engineer)
Interfaces `ILoopSource`, `ILoopAudition`, `ILoopInserter`, `LoopEntry`,
`LoopQuery` and fakes, decided by ADR-0163. No protocol change.
Reserved numbers: ADR-0163 (contract), ADR-0227 (closure). Protocol: none.
Acceptance: contract test in `common/loops` passes in PR #686.

## 4. DAW core  (rt-engineer)
Scanner, metadata parsers and filter run off the audio thread. Nothing enters
`Session::process()`.
Acceptance: engine tests in PR #686 pass.

## 5. Plugins, DSP, host  (dsp-engineer, plugin-engineer)
N/A - no processor or host change.

## 6. UI  (ui-engineer)
Loop browser model, QML panel and scene, bound to the contract in section 3.
Acceptance: UI test and screenshot in PR #686.

## 7. Tests  (tester)
Contract, engine and UI tests shipped with their slices in PR #686.

## 8. Docs and screenshots  (architect)
ADR-0163 (contract) and ADR-0227 (closure). Screenshots are not committed
(owner, 2026-11-13).

## 9. Open questions
None. Follow-ups are new backlog items, each with its own plan.
