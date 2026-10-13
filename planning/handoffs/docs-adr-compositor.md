# docs/adr-compositor

- **Goal:** decide the display stack for the Qt UI (progress item
  `system.image.kiosk`).
- **State:** in review. ADR-0010 is **Proposed** and waits for the owner. Do not merge before the owner confirms it.
- **Commits:** see `git log origin/main..docs/adr-compositor`.

## Verify
    bash tools/progress.sh --check     # expected: progress: catalogue valid, PROGRESS.md current

## Done
- ADR-0010: labwc kiosk config, Qt on Wayland, logind, five units, tty2 fallback. Evidence: `brain/decisions/ADR-0010-kiosk-compositor.md`
- Package sizes and availability measured in `debian:trixie` (2026-10-13). The ADR's Consequences table has the figures.
- Headless labwc 0.8.3 test: a fixed-size 400x300 parentless X11 window kept its size above a maximized always-on-bottom client, and an override-redirect window was drawn at its requested (50,60). The scripts lived in the session scratchpad and are described in the ADR's Rationale.
- `libpam-systemd` is not in today's image (`live/filesystem.packages`). Without it, apt picks seatd.
- Debian trixie Qt 6.8.2 has no eglfs plugin.

## Open
- Owner confirmation of ADR-0010. Then BACKLOG section "ADR-0010 display stack".
- ADR-0008/0009 are on other branches; the INDEX line for ADR-0010 may conflict textually on merge.
- Before the merge gate, delete this file and update `main-session.md` (handoff skill).

## Do not redo
- Weston: +271 MB from FreeRDP, neatvnc, GStreamer and VA-API. Rejected.
- EGLFS: not packaged in Debian. Rejected.

## Pointers
- `brain/research/adr-0007-measurements.md` §6 (the cage findings)
- `brain/research/qt-shell-render-cost.md` on local `feature/qt-shell` (65f54d1, unpushed)
