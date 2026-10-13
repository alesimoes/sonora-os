# main session

The coordinating session's handoff: what is in flight across branches. It
lives as long as the project does; overwrite it at every stopping point.
Each branch keeps its own detail in its own handoff file.

- **State:** 2026-10-12. Owner asked for token economy; harness changes on
  `feature/token-economy-harness` (PR open, awaiting review and merge).

## In flight
- `feature/token-economy-harness` — handoffs, `test-loop` and `handoff`
  skills, SessionStart hook (`.claude/settings.json` → `tools/session-start.sh`).
  Needs `code-reviewer` + `integrator`.

## Recently landed
- #6 `1031fa1` — `tools/write-usb.sh` working and guarded, tested by
  `tools/test-write-usb.sh`; boot menu timeout. Physical stick untested.

## Waiting on the owner
- Who builds the graphical UI. ADR-0006 picked Qt 6; `ui-engineer` exists;
  no agent is assigned yet.
- Phase 2 playback: 2-track monitor bus first, or full mixer (STATUS.md).

## Next
- Review and merge the harness PR, then use it: fresh agent per task and
  per review round, each starting from a handoff.
