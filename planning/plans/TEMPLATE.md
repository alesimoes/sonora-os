# Plan: <item-id> - <title>

Status: draft | approved   (approved only by architect sign-off, below)
Written by: architect   Dispatched by: lead   Architect sign-off: <name/date or pending>
Every section is filled or says `N/A - <reason>`. Each names its owner agent,
deliverables, paths and acceptance criteria a command can check.

## 1. Goal and user experience  (architect)
What the user does and sees; the flows, step by step. Out of scope.
Acceptance:

## 2. Design  (visual-designer)
Screens, faceplates, interaction specs, assets (see skill `faceplate-design`).
Deliverables / paths (`docs/images/...`, asset dirs):
Acceptance:

## 3. Contracts and integrations  (architect + owner engineer)
Headers, wire, protocol, session/format changes; ADR number; fake
implementation and contract test. Merged FIRST as its own small PR.
Deliverables / paths:
Reserved numbers (ADR, protocol, format):
Acceptance:

## 4. DAW core  (rt-engineer)
Engine, model, session work. RT-safety constraints (no alloc/lock/throw on
the audio path).
Deliverables / paths:
Acceptance:

## 5. Plugins, DSP, host  (dsp-engineer, plugin-engineer)
Processors, host changes, CPU class (ADR-0011). N/A if none.
Deliverables / paths:
Acceptance:

## 6. UI  (ui-engineer)
What it builds of section 1, bound to which contract from section 3.
Deliverables / paths:
Acceptance:

## 7. Tests  (tester)
Unit, integration, e2e to write; ctest labels; local-ci contexts touched
(`tools/local-ci.sh --dry-run` output pasted).
Deliverables / paths:
Acceptance:

## 8. QA  (tester)
Scenarios to exercise end to end; screenshots to produce under `docs/images`.

## 9. Quality checks
Sanitizers (ASan/TSan/RTSan), RT audit (rt-safety-auditor), performance and
CPU class, file size and modularity (no file past ~500 lines), code-review
focus (code-reviewer), docs to update.

## 10. Code standards
Links: `.claude/rules/code-structure.md`, `testing-and-flakes.md`,
`gates-and-checks.md`, plus area rules below.

## 11. Dependencies and order
contract PR -> parallel slices -> integrator -> one PR.
| Slice | Agent | Difficulty (S/M/L) | Risk (low/med/high) | Tier (haiku/sonnet/opus) | Sub-branch | Starts from | Files |
|---|---|---|---|---|---|---|---|

Tier from the table in `.claude/rules/agents-and-models.md`; RT, contracts,
storage, recovery and concurrency are high risk; no XL slice.
Integrator:
Single PR title:

## 12. Risks and open questions
| Question | Recommended default |
|---|---|

## 13. Done =
- [ ] contract PR merged
- [ ] every slice merged into the feature branch by the integrator
- [ ] tests of section 7 pass (commands and output in the PR)
- [ ] QA scenarios of section 8 done, screenshots committed
- [ ] quality checks of section 9 reported
- [ ] `tools/pre-pr.sh` clean; no file past ~500 lines
