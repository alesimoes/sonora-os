# ADR-0002 — Debian stable as the distribution base

- Status: Accepted
- Date: 2026-10-11

## Context

The image has to be predictable. A studio machine that boots differently after
an update is worse than one that lacks a feature. Candidates were Debian
stable, Ubuntu LTS and Arch.

## Decision

Debian stable (trixie), built with `live-build`.

## Rationale

- Frozen package set for the life of a release: the image we test is the image
  that ships. The Arch rolling model gives us a different kernel every month,
  which is the opposite of what an audio appliance wants.
- `live-build` is the native, well-documented way to produce a hybrid ISO, and
  it supports the persistence overlay we need (ADR-0005).
- `linux-image-rt-amd64` is packaged in Debian, so we do not maintain a kernel.
- Prior art: AV Linux and Ubuntu Studio both sit on this base.

## Consequences

Packages are old. Where a current PipeWire or plugin version matters, we pull
from `trixie-backports` or vendor the build — pinned explicitly, never by
tracking `unstable`.

## Amendment, 2026-11-02: one snapshot for the whole archive (designed, not built)

"Frozen for the life of a release" holds for package *names*, not for the
bytes: a trixie point release or a DSA replaces the RT kernel, so two builds
of one commit a month apart boot different kernels. Since #238 only
trixie-backports comes from `snapshot.debian.org`. Decision (recommended
default, taken unattended; the owner may revisit):

- trixie, trixie-updates and trixie-security come from the **same**
  snapshot timestamp as backports, through `lb config`:
  `--mirror-bootstrap` and `--mirror-chroot` =
  `http://snapshot.debian.org/archive/debian/<TS>/`,
  `--mirror-chroot-security` =
  `http://snapshot.debian.org/archive/debian-security/<TS>/`.
  `--mirror-binary` and `--mirror-binary-security` stay `deb.debian.org`,
  so the shipped system does not carry a frozen source.
- One timestamp, stated once (in `iso/auto/config`), checked equal to the
  one in `sonora-backports.list.chroot` by `tools/test-image-config.sh`.
  Bumping it is the only way the kernel changes.
- The security suite's Release carries `Valid-Until`, so the chroot needs
  `Acquire::Check-Valid-Until "false";` (a file under `config/apt/`), as the
  backports line already does with `check-valid-until=no`.
- **http, not https.** apt verifies the `InRelease` signature against
  `debian-archive-keyring` and every index and `.deb` against the hashes in
  it, so the transport cannot change a byte that gets installed. With expiry
  checks off, an on-path attacker could replay an *older* signed Release, but
  the URL names a fixed date and the exact version pins plus the hook's
  `dpkg-query` check fail the build on any other version. https would also
  need `ca-certificates` before the first `apt-get update`, which neither the
  debootstrap chroot nor `tools/Dockerfile.runtime` (installs it later, from
  that same source) has.
- Not yet applied: `iso/auto/config` belongs to the first ISO build in
  progress, and snapshot.debian.org is rate-limited, so a cold build through
  it has to be timed before it becomes the default. Both are BACKLOG lines.

### Applied, 2026-11-03

As designed above, with one change of place: the expiry override is
`config/archives/sonora-snapshot.conf.chroot`, not a file under
`config/apt/`. live-build copies `*.conf.chroot` into `apt.conf.d` for the
build and deletes it before packing, so the shipped system keeps checking
`Valid-Until` against the live mirrors. The timestamp is the line
`SONORA_SNAPSHOT=` in `iso/auto/config`. The chroot hook fails the build when
any build-time `deb` line is not the snapshot at that timestamp, and
`tools/build-iso.sh` drops its package cache when the timestamp changes (a
cached bootstrap would otherwise keep the old base).
