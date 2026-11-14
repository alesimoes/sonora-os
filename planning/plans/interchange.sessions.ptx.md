# Plan: interchange.sessions.ptx - .ptx import (close-out)

Docs only. No source change.

## 1. Goal and user experience  (architect)
Record what is done for .ptx read-only import and what still needs a human or
hardware. Contract, mapper and libptformat reader are on main (#678, #717).

## 2. Design  (visual-designer)
N/A - no on-screen change.

## 3. Contracts and integrations  (architect + owner engineer)
N/A - no contract change. `PtxReader` / `PtxDocument` unchanged (ADR-0156).

## 4. DAW core  (rt-engineer)
N/A - no engine change.

## 5. Plugins, DSP, host  (dsp-engineer, plugin-engineer)
N/A.

## 6. UI  (ui-engineer)
N/A.

## 7. Tests  (tester)
N/A - no code. Existing tests are untouched.

## 8. QA  (tester)
N/A.

## 9. Quality checks
`tools/pre-pr.sh` on the docs diff.

## 10. Code standards
N/A.

## 11. Dependencies and order
Depends on #678 and #717 (on main). Nothing else.

## 12. Risks and open questions
Still open, listed as unverified in ADR-0156 "Unverified":
- libptformat licence check (GPL fit) - owner to confirm.
- No free-licence `.ptx` fixture in the tree - needs a human.
- No libptformat-ON host run (`-DSONORA_WITH_LIBPTFORMAT=ON`) - needs the
  library installed and a person or hardware to run it.

## 13. Done =
- No source change in the diff.
- The three items above stay listed as unverified in ADR-0156.
