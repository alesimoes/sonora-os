# Plan: time.varispeed - Varispeed control

Status: approved
Written by: architect   Dispatched by: lead   Architect sign-off: architect 2026-11-14
Items grouped: time.varispeed only (one item, one PR). Engine half already on
main (ADR-0033, `Session::set_varispeed`, `engine/core/tests/test_varispeed.cpp`);
this feature adds the control path and UI that make it reachable.

## 1. Goal and user experience  (architect)
The user sets playback speed 50..200 % from a transport field or a key; pitch
follows (tape model); 100 % at every session open. Out of scope: recording or
looping off-speed, a better interpolation kernel, saving the ratio.
Acceptance: typing 150 in the field plays at 1.5x; reset returns to 1.0.

## 2. Design  (visual-designer)
N/A - one small field in the existing transport bar, no new faceplate.

## 3. Contracts and integrations  (architect + owner engineer)
Decision: catalog command `transport.varispeed`, argument = ratio text, via
`CommandRequest`. No new message, so no protocol bump (reserved protocol: none).
ADR-0190 amends ADR-0033 section 4. Fake engine + contract test merged first.
Unverified assumption: adding a catalog id is wire-compatible for older
peers; `test_wire` catalog loop and conformance settle it.
Reserved numbers: ADR-0190, protocol none, format none.

## 4. DAW core  (rt-engineer)
Control-thread dispatch only (`engine/control`); nothing on the audio path
changes. Parse with from_chars, refuse bad input, no throw.

## 5. Plugins, DSP, host
N/A - no processor involved.

## 6. UI  (ui-engineer)
Field + semitone readout + keymap entries, bound to `transport.varispeed`.

## 7. Tests  (tester)
`test_varispeed_command` (contracts), `test_varispeed_control` (control),
`test_transport_varispeed` (ui). Labels: contracts, control, ui.

## 8. QA  (tester)
Fake-engine run: set 150, 49, NaN text, reset; check state and refusals.

## 9. Quality checks
RT audit: confirm diff touches no file under engine/core. Files stay small
(new files). Docs: ADR-0190, INDEX line.

## 10. Code standards
`.claude/rules/code-structure.md`, `testing-and-flakes.md`, `gates-and-checks.md`.

## 11. Dependencies and order
contract -> (engine, ui in parallel) -> integrator -> one PR.
| Slice | Agent | Diff | Risk | Tier | Sub-branch | Starts from | Files |
|---|---|---|---|---|---|---|---|
| contract | rt-engineer | S | high | opus | sub/time.varispeed/contract | feat/time.varispeed | contracts/api, ADR |
| engine | rt-engineer | M | med | sonnet | sub/time.varispeed/engine | contract | engine/control |
| ui | ui-engineer | M | med | sonnet | sub/time.varispeed/ui | contract | ui/qt, keymaps |
