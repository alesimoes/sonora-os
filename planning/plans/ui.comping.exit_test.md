# Plan: ui.comping.exit_test - Comping exit test (human run)

Status: draft
Written by: engineer (eng-17) from the assignment   Dispatched by: daemon (order 1)   Architect sign-off: pending (ADR-0183 is the decision it needs)

## 1. Goal and user experience  (architect)
An engineer comps a vocal the way they do in Pro Tools: loop-record three
takes, show the lanes, make a comp, swipe across lanes, audition, consolidate,
undo. A person runs it by hand and ticks each step. The scripted
`test_exit` in `ui/qt/tests/test_comping.cpp` stays as the machine check.
Out of scope: any source change; any claim that the exit test has passed.
Acceptance: `docs/exit-tests/comping.md` lists every step with its key from
`keymaps/protools.json` and a PASS/FAIL box; ADR-0183 lists the human run as
unverified.

## 2. Design  (visual-designer)
N/A - no screen or faceplate is added; the checklist describes existing controls.

## 3. Contracts and integrations  (architect + owner engineer)
N/A - no header, wire, protocol or format change. ADR-0183 is the only
decision, reserved by the coordinator (adr=183, protocol=none).
Reserved numbers (ADR): 183.

## 4. DAW core  (rt-engineer)
N/A - no engine or session change.

## 5. Plugins, DSP, host  (dsp-engineer, plugin-engineer)
N/A - none.

## 6. UI  (ui-engineer)
N/A - no UI change. The checklist refers to the Qt window's existing controls
(lanes button, To Main, held notices).

## 7. Tests  (tester)
N/A - docs only. `tools/pre-pr.sh` is the check. No new test target.

## 8. QA  (tester)
The human run: `docs/exit-tests/comping.md` on a build, by a person, with a
connected interface. Results go in that file's boxes and the report, with the
commit. Until then the run is unverified.

## 9. Quality checks
N/A - no code. Docs: the checklist and ADR-0183 are the docs. Known doc
discrepancy (`docs/comping-a-vocal.md` says `ctrl+alt+l` shows lanes; the
keymap binds it to `clip.loop`) goes to a follow-up line, not fixed here.

## 10. Code standards
Links: `.claude/rules/gates-and-checks.md` (no check disabled; unverified is
said as unverified), `.claude/rules/planning.md`.

## 11. Dependencies and order
Single slice, docs only, one PR. Starts from the assigned sub-branch
`sub/ui.comping.exit_test/exit-checklist` (HEAD contains origin/main).

| Slice | Agent | Difficulty (S/M/L) | Risk (low/med/high) | Tier (haiku/sonnet/opus) | Sub-branch | Starts from | Files |
|---|---|---|---|---|---|---|---|
| Checklist + ADR-0183 + plan | eng-17 | S | low | haiku | sub/ui.comping.exit_test/exit-checklist | origin/main | docs/exit-tests/comping.md, brain/decisions/ADR-0183-comping-exit-test-is-a-human-run-checklist.md, planning/plans/ui.comping.exit_test.md |
