# Roadmap

Phases are sequential; work inside a phase can run in parallel. A phase closes
only when its exit criteria are *measured*, not when the code exists.

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
- RF64 for files over 4 GB.

Exit criteria: a session round-trips to another DAW through AAF with edits and
automation intact.

## Phase 5 — Hardware and hardening

- HUI and Mackie Control surfaces.
- LTC and MTC sync, word clock and ADAT.
- Read-only root with an overlay, A/B updates, crash recovery of a session.

Exit criteria: 24 hours of continuous recording with zero xruns, and a session
recovered intact after the power is pulled mid-take.
