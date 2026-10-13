# main session

The coordinating session's handoff: what is in flight across branches. It
lives as long as the project does; overwrite it at every stopping point.
Each branch keeps its own detail in its own handoff file.

- **State:** 2026-10-12, written to be true once the Qt shell PR (#10) has merged.

## Verify
    git log --oneline -6 origin/main   # expected: the Qt shell squash on top of 0cedb56 (#9)
    tools/progress.sh --summary        # expected: one "Progress: N% overall" line

## Recently landed
- #2–#6: key maps and take safety; ADR-0007 + measurements + M1 notes; M1
  module layout; `tools/write-usb.sh`.
- #7: handoffs, `handoff`/`test-loop` skills, SessionStart hook.
- #8: progress catalogue, `tools/progress.sh`, `planning/PROGRESS.md`.
- ADR-0008: portability — platform code at the edges; ASIO is the future
  Windows backend; Linux appliance first (owner, 2026-10-12).
- #10: Qt 6 shell — 18 scene-graph meters, transport status, key routing by
  physical digit keys (US/AZERTY/ABNT2 synthetic tests); llvmpipe-only cost
  figures, no GPU measurement yet; no compositor in the image yet.

## In flight
- `contract/backend` — ADR-0007 M2, `rt-engineer`: device-selection policy
  (prefer USB, ignore onboard unless named) and final review fixes, then PR.
- `feature/usb-and-quantum-followups` — `distro-engineer`: pin quantum 128;
  `write-usb.sh` follow-ups.
- `docs/phase2-session-timeline` — `architect`: session format + timeline spec.
- `docs/adr-compositor` — `architect`: kiosk compositor ADR (Proposed).
- `test/bwf-interop` — `researcher`: WAVs open in other tools at the right
  timecode.

## Decided by the owner (2026-10-12)
- Phase 2 playback starts with a fixed 2-channel monitor bus.
- Portability rule (ADR-0008); Windows via ASIO later, not now.

## Waiting on the owner
- `CLAUDE.md` layout table still pre-M1 (owner's edit, ADR-0007 M6).
- First boot on real hardware with the UMC1820 (only the owner can do it).

## Next
- M2 merge → M3 fake backend + ADR-0008 P1–P3 → playback (Phase 2) → M4.
