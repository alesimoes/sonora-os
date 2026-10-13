# main session

The coordinating session's handoff: what is in flight across branches. It
lives as long as the project does; overwrite it at every stopping point.
Each branch keeps its own detail in its own handoff file.

- **State:** 2026-10-13, written to be true once `feature/ui-portable-ci` (#12) has merged.

## Verify
    git log --oneline -6 origin/main   # expected: the ui-portable-ci squash on top of 87d9f61 (#13)
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
- #13: `tools/test-bwf-interop.sh` + CI job — takes open at their bext
  TimeReference in Ardour 8.12, ffprobe, soxi, libsndfile. Known: bext date/time
  empty, 4 GB wrap (fix/bwf-writer in flight); TimeReference 0 = "no timestamp"
  in Ardour (session start 01:00:00:00 in the Phase 2 spec addresses it).
- #12 `feature/ui-portable-ci`: ADR-0008 P5 — `ui/qt` is Qt + C++17 only;
  `common/platform` created (`thread_cpu_times()`, posix + unsupported);
  CI job `ui` builds `ui/qt` on ubuntu-24.04 apt Qt 6.4 and runs its
  headless tests, the Xvfb pixel test included (never `--measure`).

## In flight
- `fix/bwf-writer` — `rt-engineer`: bext OriginationDate/Time, refuse the
  4 GB wrap with a take failure, RIFF pad byte.
- `contract/backend` — ADR-0007 M2, `rt-engineer`: device-selection policy
  (prefer USB, ignore onboard unless named) and final review fixes, then PR.
- `feature/usb-and-quantum-followups` — `distro-engineer`: pin quantum 128;
  `write-usb.sh` follow-ups.
- `docs/phase2-session-timeline` — `architect`: session format + timeline spec.
- `docs/adr-compositor` — `architect`: kiosk compositor ADR (Proposed).

## Decided by the owner (2026-10-12)
- Phase 2 playback starts with a fixed 2-channel monitor bus.
- Portability rule (ADR-0008); Windows via ASIO later, not now.

## Waiting on the owner
- `CLAUDE.md` layout table still pre-M1 (owner's edit, ADR-0007 M6).
- First boot on real hardware with the UMC1820 (only the owner can do it).

## Next
- M2 merge → M3 fake backend + ADR-0008 P1–P3 → playback (Phase 2) → M4.
