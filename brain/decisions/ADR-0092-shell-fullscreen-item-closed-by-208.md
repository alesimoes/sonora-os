# ADR-0092 - `ui.shell.fullscreen` is already delivered by ADR-0010 / #208

- Status: Accepted
- Date: 2026-11-08
- Item: `ui.shell.fullscreen`
- Relates to: ADR-0010 (kiosk compositor), ADR-0006, ADR-0091 (same pattern)

## Context

The item "Qt shell fullscreen in the image, no desktop visible" is still
`todo`. Main already does it, by ADR-0010 section 1-2: labwc's `rc.xml`
window rule for `sonora-ui` (`serverDecoration="no"`, `fixedPosition="yes"`,
`Maximize`, `ToggleAlwaysOnBottom`), the UI's `app_id` set with
`setDesktopFileName` (#208), no root menu or desktop bindings,
`sonora-ui.service` running `sonora-ui --kiosk`. `tools/test-labwc-kiosk.sh`
and `tools/test-image-config.sh` check it.

## Decision

No new design. The UI does not request full screen itself; the compositor
maximizes it (ADR-0010 section 2). The item closes by reference; the only
work is this ADR and its INDEX line.

## Rationale

A client-side `showFullScreen()` would duplicate the compositor rule and
behave differently from the always-on-bottom editors policy.

## Consequences

Unverified: that the maximized, undecorated surface shows on real hardware
and on the current ISO (no desktop flash beyond the ~1 s restart gap ADR-0010
accepts). Settled by `tools/test-boot-qemu.sh` on a fresh ISO with a
screenshot (release/nightly, ADR-0061); red means a new fix task.

## Alternatives rejected

- `showFullScreen()` in Qt: bypasses the always-on-bottom rule, so an
  editor could be hidden behind the UI.
- Leaving the item `todo`: it would be dispatched again.
