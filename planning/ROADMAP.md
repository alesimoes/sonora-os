# Roadmap

Phases are sequential; work inside a phase can run in parallel. A phase closes
only when its exit criteria are *measured*, not when the code exists.

Exception, decided by the owner 2026-10-12 (ADR-0007 §10): some Phase 3 and
Phase 4 work is open early, run by parallel squads.
- Phase 3: only what has engine state today (shell, transport, meters,
  arming, key routing), with no timeline editing.
- Phase 4: plugin work host-side and measurement only, with nothing in
  `engine/`.

Neither phase is open beyond that, and Phase 1 still closes only on its own
measured exit criteria.

## Phase 0 — Foundations (done)

Repository, harness, ADRs, real-time audio core, BWF writer, unit tests.

Exit criteria: `tools/build-engine.sh` builds clean and `ctest` passes.

## Phase 1 — Bootable appliance

- `live-build` configuration producing a hybrid ISO.
- RT kernel, PipeWire pinned quantum, RT limits.
- Boot with no login into the engine, fullscreen, no desktop visible.
- Automatic detection of a class-compliant USB interface.
- Persistence partition, `tools/write-usb.sh`.

Exit criteria: a USB stick boots on two different machines and records 8 tracks
for 10 minutes with zero xruns at 128 frames. The resulting files open in
another DAW at the right timecode.

## Phase 2 — The engine becomes a DAW

- Playback and mixing, not just capture. Monitoring with input latency
  compensation.
- Sample-accurate transport: locate, loop, punch in and out, pre-roll.
- Timeline data model with undo. Regions, fades, crossfades.
- Session file format, documented in `brain/specs/` before it is written.

Exit criteria: record, play back, punch in over an existing take, and undo it,
without stopping the transport.

## Phase 3 — Workflow layer

- Full key map plus swappable profiles (`keymaps/protools.json`,
  `keymaps/logic.json`).
- Edit modes, Smart Tool, grid and nudge.
- Playlists and comping.
- Mixer: inserts, sends, VCA, groups.

Exit criteria: an engineer who works in Pro Tools can comp a vocal without
reading documentation.

## Phase 4 — Interchange and plugins

- AAF import and export through `libaaf`.
- `.ptx` read-only through `libptformat`.
- LV2 and VST3 hosting, out of process so a plugin crash cannot take the
  session down.
- **Parallel processing graph with plugin delay compensation.** The requirement
  is every track playing with its plugin chain in real time, latency
  synchronised. At quantum 128 that is a 2.67 ms budget for 18 tracks of
  plugins, so the graph is parallel from the start and plugins are dropped on
  deadline rather than waited on. Designed in
  `brain/specs/plugin-latency-compensation.md`. PDC is cheap to design for and
  very expensive to retrofit.
- Freeze and bounce, so a chain too expensive for real time still has a path.
- RF64 for files over 4 GB.

Exit criteria: a session round-trips to another DAW through AAF with edits and
automation intact. Separately: 18 tracks with four plugins each play at quantum
128 with zero xruns, and a track carrying 2048 samples of plugin latency lands
sample-aligned with a track carrying none.

## Phase 5 — Hardware and hardening

- HUI and Mackie Control surfaces.
- LTC and MTC sync, word clock and ADAT.
- Read-only root with an overlay, A/B updates, crash recovery of a session.

Exit criteria: 24 hours of continuous recording with zero xruns, and a session
recovered intact after the power is pulled mid-take.

## Phase 6 — Installable to disk

Requirement stated 2026-10-11: install it on a laptop and run it from there,
not only from the live medium.

- An installer: partition, copy the system, install the bootloader, create the
  operator account, carry over any session on the stick.
- A real root filesystem rather than a squashfs overlay, so sessions write
  straight to disk at full speed. A live medium is the wrong place to record a
  32-track session: about 4.6 MB/s sustained while also serving the root.
- Swap sized for the machine, and a filesystem tuned for audio: no `atime`,
  and a journal that does not stall the disk thread.
- Updates on an installed system, a different problem from the A/B updates
  Phase 5 does for the image.
- UEFI entry and Secure Boot. Unsigned is tolerable for a stick and awkward
  for an installed machine.

`--debian-installer none` is set in `iso/auto/config` today. Whether the
installer is Debian's, Calamares, or something small of our own is an ADR, not
a decision to drift into. An appliance installer has one job and should not
offer a desktop-install decision tree.

Exit criteria: install to an internal disk, boot from it with the live medium
removed, and record a session that survives a reboot.
