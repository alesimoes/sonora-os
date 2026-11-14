# ADR-0231 - `ui.shell.fullscreen.r1114` closed: already delivered (see ADR-0092)

- Status: Accepted
- Date: 2026-11-14
- Item: `ui.shell.fullscreen.r1114`
- Relates to: ADR-0010 (kiosk compositor), ADR-0092 (`ui.shell.fullscreen` closed by #208)

## Context

The r1114 item asks for the Qt shell to run fullscreen in the image with no
desktop visible. ADR-0092 already records that this is delivered: labwc's
`rc.xml` window rule for `sonora-ui` sets `serverDecoration="no"` and
`fixedPosition="yes"`, and maximizes the window.

## Verification (2026-11-14, no code change)

- `grep -n Maximize iso/config/includes.chroot/usr/share/sonora/labwc/rc.xml`
  matches line 42: `<action name="Maximize"/>` inside the `sonora-ui`
  window rule.
- `grep ADR-0092 brain/INDEX.md` matches the index entry for ADR-0092.

## Decision

Close `ui.shell.fullscreen.r1114` by reference to ADR-0092 and ADR-0010. No
code, no image change. The real-hardware look stays unverified until the
hardware validations parked for the end of the project.
