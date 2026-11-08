# Plan: drivers.backends.alsa - Direct ALSA backend

Status: approved
Written by: architect   Dispatched by: lead   Architect sign-off: architect 2026-11-08

## 1. Goal and user experience  (architect)
Already delivered by #435 (a57ed021) under ADR-0053: `drivers/alsa/` and `--backend alsa` with `--alsa-period/--alsa-periods`. Out of scope: new code.
Acceptance: `git ls-files drivers/alsa | grep alsa_backend` lists header and source.

## 2. Design
N/A - no UI.

## 3. Contracts and integrations
ADR-0094 (reserved, written by the architect, closes the item). No protocol or format change.
Reserved numbers: ADR 94, protocol none, format none.
Acceptance: ADR-0094 file exists; its INDEX line is added by the coordinator at merge (INDEX.md is claimed by other features).

## 4. DAW core
N/A - no engine change.

## 5. Plugins, DSP, host
N/A.

## 6. UI
N/A.

## 7. Tests
N/A - existing `alsa_*` ctest targets cover it against a fake card.

## 8. QA
Unverified: real-card xruns on the target UMC1820 (needs hardware). Hand to the researcher: a loopback or hardware run at 64/128 frames, counting xruns.

## 9. Quality checks
`tools/pre-pr.sh`. Docs only.

## 10. Code standards
`.claude/rules/gates-and-checks.md`.

## 11. Dependencies and order
Grouped: only this item (the other backend items are separate contexts).
| Slice | Agent | Difficulty | Risk | Tier | Sub-branch | Starts from | Files |
|---|---|---|---|---|---|---|---|
| close | driver-engineer | S | low | junior | feat/drivers.backends.alsa | main | brain/decisions/ADR-0094-alsa-backend-item-closed-by-435.md |
Single PR title: Close drivers.backends.alsa by reference to #435 (ADR-0094)

## 12. Risks and open questions
| Question | Recommended default |
|---|---|
| Does it hold on real hardware? | Researcher measures; red = new fix task |
