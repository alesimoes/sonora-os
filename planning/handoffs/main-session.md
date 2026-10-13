# main session

The coordinating session's handoff: what is in flight across branches. It
lives as long as the project does; overwrite it at every stopping point.
Each branch keeps its own detail in its own handoff file.

- **State:** 2026-10-12, written to be true once the harness PR (#7) has
  merged. `main` before it was `1031fa1`.

## Verify
    git log --oneline -8 origin/main   # expected: the harness squash on top of 1031fa1
    git branch -r                      # expected: the three in-flight branches below

## Recently landed
- #2–#6, up to `1031fa1`: key maps and take safety; ADR-0007 with its
  measurements and M1 notes; M1 module layout; `tools/write-usb.sh`.
- #7: handoffs, `handoff` and `test-loop` skills, SessionStart hook
  (`.claude/settings.json` → `tools/session-start.sh`). Not yet seen firing
  in a fresh `claude` session; the first one should start with the data line.

## In flight
- `contract/backend` — ADR-0007 M2, `rt-engineer`. Reviewed: RT gate passed;
  the driver co-owner asked for contract changes, being applied.
- `feature/qt-shell` — `ui-engineer`, Qt 6 shell and meters. Not yet reviewed.
- `feature/usb-and-quantum-followups` — `distro-engineer`: pin the PipeWire
  quantum to 128; `write-usb.sh` follow-ups.

## Waiting on the owner
- Phase 2 playback: 2-track monitor bus first, or the full mixer
  (`planning/STATUS.md`).
- `CLAUDE.md` layout table still shows the pre-M1 layout: the owner's edit,
  per ADR-0007 M6.

## Next
- M2 PR → M3 fake backend → M4 control API (the UI hooks onto it).
- Fresh agent per task and per review round, each starting from a handoff.
