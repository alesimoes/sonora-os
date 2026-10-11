# Brain index

One line per entry. Agents read this before opening anything else.

## Decisions
- [ADR-0001](decisions/ADR-0001-engine-from-scratch.md) — engine written from scratch, not a fork of Ardour.
- [ADR-0002](decisions/ADR-0002-debian-base.md) — Debian stable as the distribution base.
- [ADR-0003](decisions/ADR-0003-pipewire-jack-api.md) — PipeWire as the server, JACK API as our client interface.
- [ADR-0004](decisions/ADR-0004-no-proprietary-reverse-engineering.md) — workflow cloning is in scope, binary reverse engineering is not.
- [ADR-0005](decisions/ADR-0005-usb-boot-persistence.md) — hybrid ISO plus a persistence partition for projects.

## Specs
- [BWF](specs/bwf-broadcast-wave.md) — the `bext` chunk layout we write, field by field.

## Research
_(empty — first notes land when the researcher agent runs)_

## Postmortems
- [2026-10-11 debootstrap tar failure](postmortems/2026-10-11-livebuild-tar-failure.md) — live-build cannot build a chroot on a Windows bind mount; also caught a masked exit code and a missing engine install.
