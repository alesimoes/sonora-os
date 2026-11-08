# Plan: ui.mixer.folders - Folder tracks and track stacks (basic)

Status: approved
Written by: architect   Dispatched by: lead   Architect sign-off: architect 2026-11-08

## 1. Goal and user experience  (architect)
Already delivered by ADR-0048 section 4 / #438: Folder in the edit state, folder.new / folder.dissolve, Mix tags and closed card, session format 20. Out of scope: new code; a summing-bus folder and nesting (ADR-0048 "not done here") become a follow-up BACKLOG line.
Acceptance: `grep -n folder_new ui/qt/src/edit_vcas.cpp` and `ls docs/images/sonora-ui-mix-folders.png`.

## 2. Design
N/A - no UI change.

## 3. Contracts and integrations
ADR-0093 (reserved, written by the architect). No protocol or format change.
Reserved numbers: ADR 93, protocol none, format none.
The INDEX.md line is added by the coordinator at merge (other active features claim that file).

## 4. DAW core
N/A.

## 5. Plugins, DSP, host
N/A.

## 6. UI
N/A.

## 7. Tests
N/A - `common/model/tests/test_vcas_folders.cpp` exists.

## 8. QA
Unverified: that the folder scene still renders on current main (nightly screenshots job).

## 9. Quality checks
`tools/pre-pr.sh`. Docs only.

## 10. Code standards
`.claude/rules/gates-and-checks.md`.

## 11. Dependencies and order
Grouped: only this item; ui.mixer.vca shares ADR-0048 but is a separate open line and not claimed here.
| Slice | Agent | Difficulty | Risk | Tier | Sub-branch | Starts from | Files |
|---|---|---|---|---|---|---|---|
| close | ui-engineer | S | low | junior | feat/ui.mixer.folders | main | brain/decisions/ADR-0093-mixer-folders-item-closed-by-438.md |
Single PR title: Close ui.mixer.folders by reference to ADR-0048 / #438 (ADR-0093)

## 12. Risks and open questions
| Question | Recommended default |
|---|---|
| Summing-bus folder wanted? | Separate follow-up with its own ADR |
