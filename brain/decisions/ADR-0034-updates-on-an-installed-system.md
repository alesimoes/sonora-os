# ADR-0034 — Updates on an installed system: apt with holds now, A/B later

- Status: Accepted (recommended default, taken with no owner available,
  2026-11-04)
- Date: 2026-11-04
- Item: `system.install.updates`
- Relates to: ADR-0002 (Debian base), ADR-0005 (USB persistence), ADR-0032
  (installer). Number 0034: 0033 is claimed by varispeed; the Secure Boot
  ADR should take a higher one.

## Summary

The installed system updates with Debian's own apt, from Debian stable only,
and only when the operator asks. The RT kernel and Sonora's own packages are
held: they move together in a Sonora release, never through `apt upgrade`.
Nothing updates by itself, and nothing updates while a session is open.
Studios without internet update from a signed stick carrying a local apt
repository. Rollback for the kernel is the previous kernel kept in GRUB;
rollback for everything else waits for the A/B item
(`system.hardening.ab_updates`), which replaces this ADR's mechanism, not
its rules. Only the policy files are verified (statically); no installed
machine has run an update.

## Decision

1. **apt, not image-based, for now.** The installer copies the tested
   squashfs (ADR-0032), so the installed root is an ordinary Debian root.
   Image-based updates need the A/B partition layout, which the installer
   does not create yet. Doing both now would be two mechanisms. A/B stays a
   separate item and replaces this one when it lands.
2. **Sources.** Debian stable and its security archive, with signed Release
   files. Nothing from testing, unstable or experimental (pinned to -1 in
   `/etc/apt/preferences.d/sonora.pref`). No third-party repositories.
3. **Held.** `linux-image-*`, `linux-headers-*` and the `rt-amd64` metas have
   pin -1: apt cannot pick a new kernel. A Sonora release installs the
   kernel it was tested with, keeps the previous one, and GRUB lists both.
   The same rule applies to the `sonora-*` packages once they exist.
4. **Never mid-session.** `apt-daily` and `apt-daily-upgrade` are conditioned
   on `/var/lib/sonora/updates-allowed`, a flag the update tool creates only
   when the engine reports no open session and removes afterwards. Periodic
   apt settings are all off and there is no unattended-upgrades. No update
   reboots by itself.
5. **Offline.** A stick holds a local apt repository (Release signed by the
   Sonora key, the key in `/usr/share/keyrings`) plus the kernel. The
   updater adds it as a `file:` source for the one run. Same pins, same flag.
6. **Rollback.** Kernel: choose the previous GRUB entry; the updater keeps
   two. Packages: `apt install pkg=version` from the cache the updater keeps
   until the next successful boot. Anything beyond that is the A/B item.
7. **Read-only root.** If the root is mounted read-only, the updater remounts
   it read-write for the run and read-only after, and refuses if it cannot
   remount back.

## Unverified

Everything beyond the files: the updater tool is not written, the flag
handshake with the engine does not exist, the offline repository and key are
not built, and no installed machine has taken an update.

## Consequences

Security fixes from Debian reach the machine only when the operator acts.
That is the intended trade for a recording appliance. The policy files ship
in the image and `tools/test-updates.sh` checks them.
