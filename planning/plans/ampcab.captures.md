# Plan: ampcab.captures - Amp and cabinet captures (close-out)

Decision record: `brain/decisions/ADR-0221-ampcab-captures-closure.md`.
Docs only: this plan changes `planning/` and `brain/`, no source.

## 1. Goal and user experience
N/A for a close-out. The goal is to record that the capture tooling is complete
on `main` and that the captures themselves wait on the owner's hardware.

## 2. Design
N/A: no faceplate or sound change.

## 3. Contracts and integrations
N/A: ADR-0069's contract (`capture_card.h`, the manifest format) is unchanged.

## 4. DAW core
N/A: no engine change.

## 5. Plugins, DSP, host
N/A: no `common/dsp` or `plugins/native/ampcab` source change. No capture file
is produced, pinned or shipped.

## 6. UI
N/A: no UI change.

## 7. Tests
N/A: nothing new to test. The existing `native_ampcab_card_contract` and the
card checker stay as they are on main.

## 8. QA
N/A: no behaviour changes.

## 9. Quality checks
- The diff touches only `planning/` and `brain/` (`git diff --name-only origin/feat/ampcab.captures`).
- `tools/pre-pr.sh` run before push.

## 10. Code standards
N/A: no code.

## 11. Dependencies and order
- ADR-0069 and the rig note are on main (#534). Nothing else is waited on.
- Each future capture is its own PR and waits on the owner supplying the unit.

## 12. Risks and open questions
- Owner hardware: which units, and whether recording is allowed (rental terms).
  Waits on the owner; no default can stand in for a recording.
- `ampcab.budget` re-run on a quiet machine stays open under its own item.

## 13. Done =
- ADR-0221 and this plan are on the branch.
- `brain/INDEX.md` lists ADR-0221.
- The diff touches only `planning/` and `brain/`.
- The backlog line is ticked by the coordinator's chore PR as "tooling done;
  captures parked to owner hardware", not as a plain `[x]`.
