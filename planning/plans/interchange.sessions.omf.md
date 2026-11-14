# Plan: interchange.sessions.omf - Close out OMF import (already on main)

Status: draft   (approved only by architect sign-off, below)
Written by: eng-17 (docs closure)   Dispatched by: lead   Architect sign-off: pending
ADR: 222 (`brain/decisions/ADR-0222-omf-import-closure.md`)

## 1. Goal and user experience  (architect)
The OMF import item is closed. Importer, mapper, reader (#679) and
`sonora-convert omf-import` (#713) are on main. The operator gets nothing new;
the record says accurately what is verified and what is not.
Out of scope: any code change; real-world OMF files (need a human to supply
them); OMF export (not planned, ADR-0160 decision 7).
Acceptance:
- ADR-0222 exists and states the closure and the remaining unverified items.
- ADR-0160's `## Unverified` section is unchanged and still lists real-world
  fixtures.

## 2. Design  (visual-designer)
N/A - no on-screen change.

## 3. Contracts and integrations  (architect + owner engineer)
N/A - no contract change. The OMF contract (`omf_document.h`, `omf_import.h`)
is merged and unchanged.

## 4. DAW core  (rt-engineer)
N/A - no engine or real-time change.

## 5. Plugins, DSP, host  (dsp-engineer, plugin-engineer)
N/A - no plugin or DSP change.

## 6. UI  (ui-engineer)
N/A - no UI change.

## 7. Tests  (tester)
N/A - no test added or changed. The existing OMF tests
(`interchange/omf/tests/`) are the evidence cited, not re-run here.

## 8. QA  (tester)
N/A - docs only; no behaviour to test.

## 9. Quality checks
- `tools/pre-pr.sh` (whitespace, ADR number against main and open PRs).
- `git diff --name-only origin/main` lists only the two claimed files.

## 10. Code standards
N/A - no code. Markdown only.

## 11. Dependencies and order
Depends on #679 and #713 (both on main). No dependency on an open PR.

## 12. Risks and open questions
- Real-world OMF variants may differ from the spec; the reader would then need
  fixes. Default: stays listed as unverified in ADR-0160 until a file exists.
- `interchange/omf/README.md` says the `sonora-convert` wire-up is still a
  follow-up; #713 did it. Default: follow-up line, not edited here (outside the
  claimed files).
- Redistribution terms of the OMFI specification (ADR-0160). Default: owner or
  researcher to confirm; not blocking the closure.

## 13. Done =
- No source change in the diff.
- ADR-0160's Unverified section still names real-world file fixtures.
- ADR-0222 and this plan committed on `sub/interchange.sessions.omf/omf-closure`.
