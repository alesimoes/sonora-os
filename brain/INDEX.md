# Brain index

One line per entry. Agents read this before opening anything else.

## Decisions
- [ADR-0001](decisions/ADR-0001-engine-from-scratch.md) — engine written from scratch, not a fork of Ardour.
- [ADR-0002](decisions/ADR-0002-debian-base.md) — Debian stable as the distribution base.
- [ADR-0003](decisions/ADR-0003-pipewire-jack-api.md) — PipeWire as the server, JACK API as our client interface.
- [ADR-0004](decisions/ADR-0004-no-proprietary-reverse-engineering.md) — workflow cloning is in scope, binary reverse engineering is not.
- [ADR-0005](decisions/ADR-0005-usb-boot-persistence.md) — hybrid ISO plus a persistence partition for projects.
- [ADR-0006](decisions/ADR-0006-qt6-ui-toolkit.md) — Qt 6 for the shell, custom GPU scene-graph items for meters and timeline; the UI pulls at 60 Hz, the audio thread never pushes.

## Specs
- [BWF](specs/bwf-broadcast-wave.md) — the `bext` chunk layout we write, field by field.

## Research
- [Multichannel USB interfaces](research/multichannel-usb-interfaces.md) — Pro Audio profile, WirePlumber state hooks, port ordering; what is verified and what is not.

## Postmortems
- [2026-10-11 comment in a line continuation](postmortems/2026-10-11-comment-in-line-continuation.md) — dropped half a QEMU command line; bash -n cannot catch it. Also: an edit that reported success without landing.
- [2026-10-11 ring buffer data race](postmortems/2026-10-11-ringbuffer-data-race.md) — producer used the consumer's memory ordering; every test passed, TSan did not.
- [2026-10-11 RT kernel not default](postmortems/2026-10-11-rt-kernel-not-default.md) — the image installed the RT kernel but booted the generic one; no symptom except unexplained xruns.
- [2026-10-11 debootstrap tar failure](postmortems/2026-10-11-livebuild-tar-failure.md) — live-build cannot build a chroot on a Windows bind mount; also caught a masked exit code and a missing engine install.
