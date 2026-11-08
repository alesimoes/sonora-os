# ADR-0091 - `drivers.usb.virtual_card` is already delivered by #354

- Status: Accepted
- Date: 2026-11-08
- Item: `drivers.usb.virtual_card`
- Relates to: ADR-0089 (same pattern), ADR-0007

## Context

The item "Virtual-card QEMU test: snd-dummy rme9652 through ALSA ->
PipeWire -> JACK, recording" was still `todo` when it was planned. Main
already has it: `tools/test-virtualcard-qemu.sh` and the image script
`iso/config/includes.chroot/usr/local/bin/sonora-virtualcard-test`
(commit e6f1e033, PR #354).

## Decision

No new design. The item closes by reference to #354. The only work left is
the bookkeeping PR: this ADR and its INDEX line.

## Rationale

Re-implementing would duplicate a merged test and risk a second, drifting copy.

## Consequences

The test needs an ISO and QEMU, so it is a release/nightly check, not a PR gate
(ADR-0061). Unverified: that it passes on the current main image; the
coordinator or researcher must run `tools/test-virtualcard-qemu.sh` on a fresh
ISO and report. If red, that is a new fix task.

## Alternatives rejected

- A fresh implementation: duplicates #354.
- Leaving the item `todo`: it would be dispatched again.
