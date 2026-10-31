# Roadmap

Two milestones, in order. A stage closes only when its exit criteria are
*measured*, not when the code exists. Work inside a stage, and across stages
whose dependencies are met, runs in parallel.

## Order of work, decided by the owner 2026-10-13

> "I want to see the product — the DAW running, recording, including
> plugins, like Logic and Pro Tools; then comes the OS to install."
> Corrected the same day: "It's not a DAW MVP, it's a complete product",
> with "all the features we have in Pro Tools and Logic".

1. **Complete DAW** first: the whole product, feature by feature, at parity
   with the workflow and behaviour Avid and Apple document for Pro Tools and
   Logic Pro. Its stages are ordered by dependency. Each stage is a step
   toward the complete product, not a reduced version of it.
2. **OS packaging** after: the bootable image, kiosk, USB stick, target
   hardware, hardening, installer. Nothing already built there is thrown
   away. It waits.

What follows from the decision:

- **Where it runs until then.** Development and demos run on the owner's
  Windows laptop under WSL2 (Debian trixie, WSLg for the Qt window and for
  audio), on the laptop's onboard audio. `tools/dev-wsl.sh` sets it up, and
  is written when engine and UI are first connected.
- **No ISO, QEMU, USB or installer work** until the Complete DAW exists. The
  CI jobs that already run stay green; nobody extends them.
- **The UMC1820 is connected at the end**, in OS packaging. Every hardware
  figure (latency, xruns at quantum 128, 18 channels, the ADR-0009
  measurements on a stick) moves there. Until then the laptop gives proxy
  figures and they are labelled as proxies.
- **ADR-0007 §10's early scope limit is lifted entirely** for the Complete
  DAW (owner amendment dated 2026-10-13 in that ADR). The UI builds editing
  and the mixer against `common/model` and `FakeEngine`; plugin work goes
  into `engine/plugin_client` once M4 exists.
- **ADR-0009's "before M8, needs hardware" measurements** cannot precede M8
  any more. They run as laptop proxies before M8 merges and on hardware in
  OS packaging (the ADR-0009 update of 2026-10-13 records the risk).

### Where the feature list comes from

Every Complete DAW item in `planning/progress.json` carries a `ref` naming
the published documentation its behaviour is taken from:

- **PT-RG**: Avid, *Pro Tools Reference Guide*, with the Shortcuts Guides
  already used in `brain/research/protools-logic-key-bindings.md`. The
  edition to cite is not yet pinned (the newest one seen is 2024.3, on a
  third-party mirror); pinning it is a researcher item in the backlog.
- **LP-UG**: Apple, *Logic Pro for Mac User Guide*, 12.3, the same source
  as LP-A in that research note.

ADR-0004 holds throughout: we reproduce workflow and behaviour from published
documents, never code, binaries, assets or content. Where the Pro Tools or
Logic feature *is* content (instrument sample libraries, Apple Loops,
Drummer and Session Players, Alchemy's sound banks, Apple's own effects),
the catalogue marks it **"via plugins/content"**: we host third-party
instruments and effects (LV2, VST3, Windows VST through yabridge) and ship a
curated set of free-licensed plugins and our own content. We do not
reimplement Apple's or Avid's instruments, and we never ship their content.

Out of scope, said once so nobody schedules it: EuCon (Avid's SDK is
licensed, not published), Splice and other cloud services, Avid's and
Apple's own content, Dolby's renderer without Dolby's licence.

### The honest scale

Pro Tools and Logic are decades of work by large teams. Parity with both is
a **multi-year scope** for this project. On 2026-10-13 the catalogue holds
about 595 points of Complete DAW work against about 60 done, and most of
the done points are the cheap foundations. The stages exist so that the
order is right and every stage leaves a product the owner can run and use,
not so that the scale looks smaller. Points are not a schedule. Nobody
should extrapolate a date from the first 60.

### Phase numbers before 2026-10-13

ADRs, specs and the backlog cite the old phases. They keep their text. This
is where each one's content went:

| Old phase | Now |
|---|---|
| 0 Foundations | done; stage 0 |
| 1 Bootable appliance | contracts and migration (M2 to M7): DAW 1 and 2. Image, kiosk, USB stick, hardware exit: OS 1 |
| 2 The engine becomes a DAW | DAW 1 |
| 3 Workflow layer | DAW 2 (editing, key maps, mixer basics), DAW 3 (comping), DAW 4 (full mixer) |
| 4 Interchange and plugins | DAW 5 (plugins, PDC, freeze), DAW 8 (interchange) |
| 5 Hardware and hardening | DAW 10 (surfaces, sync). OS 1 (hardware measurements) and OS 2 (read-only root, A/B updates, power-pull recovery, 24-hour soak) |
| 6 Installable to disk | OS 3 |

MIDI was absent from every old phase. It is DAW 6.

---

# Milestone 1 — Complete DAW

Exit criterion for the milestone: every item of the "Complete DAW" front in
`planning/progress.json` is done with evidence, and each stage's exit
criterion below has been measured on the owner's laptop under WSL2 with
onboard audio. `tools/progress.sh` shows the milestone's percentage per
module.

## Stage 0 — Foundations (done)

Repository, harness, ADRs, real-time audio core, BWF writer, unit tests, the
module layout (M1), the Qt shell over a synthetic engine.

Exit criteria: `tools/build-engine.sh` builds clean and `ctest` passes.

## DAW 1 — Engine core, contracts, model, session file

The critical path. Everything else integrates against it.

- ADR-0007 M2 (backend contract) → M3 (`drivers/fake`, conformance suite)
  → M4 (control API, `StateView`, `InProcessEngine`, `FakeEngine`; the
  state block per §3) → M5 → M6.
- `common/json` and `common/model` (ADR-0009, M8): snapshots, operations,
  undo, region operations, the session reader and writer.
- Playback through the residency-gated plan swap and the **fixed 2-channel
  monitor bus** (owner, 2026-10-12): every track feeds it, panned, with its
  fader and mute. The full mixer is DAW 4, built on the same graph.
- Sample-accurate transport: locate, loop, punch in and out, pre-roll.
  Recording while rolling with in-band take boundaries. Input monitoring
  with latency compensation.
- Losing a take is the worst outcome: the 4 GB limit refused rather than
  wrapped, periodic `fdatasync`, a take that survives `kill -9` of the
  engine.

Exit criteria (the old Phase 2 criterion, unchanged): record, play back,
punch in over an existing take, and undo it, without stopping the
transport, as a ctest on `drivers/fake` with captured outputs that compares
the output sample for sample.

## DAW 2 — Editing and mixing in the Qt UI, on the laptop

- The Qt UI wired to M4. M7 (process split) lands in this stage; the UI may
  connect through `InProcessEngine` first.
- Edit window: tracks, rulers, waveforms, regions. Move, trim, split, edit
  commands, selection, zoom. Edit modes (Shuffle, Slip, Spot, Grid), Smart
  Tool, grid and nudge. Fades and crossfades, clip gain, names and colours,
  markers and memory locations, undo history, audio import.
- Tempo map, time signatures, bars|beats grid, click and count-in.
- Mixer basics on the monitor bus: fader, pan, mute, solo, metering.
- Transport window and counters. Pro Tools and Logic key maps driving M4
  commands, switchable from the UI. Edit and Mix windows, preferences.
- `tools/dev-wsl.sh`, after the researcher has measured WSLg audio.

Exit criteria, on the laptop under WSL2 with onboard audio, from a session
started by `tools/dev-wsl.sh`: record at least 4 tracks in two passes, the
second overdubbed while the first plays; move, trim and split a region, undo
all three and redo them, once with the Pro Tools profile's keys and once
with Logic's; set fader, pan, mute and solo on each track; save, quit,
reopen, and the canonical session file is byte-identical. A 10-minute
record-and-play pass reports zero overruns and zero xruns at the quantum the
WSLg measurement found stable.

## DAW 3 — Recording workflow: playlists and comping

- Punch and loop recording from the UI, auto input monitoring.
- Playlists per track, loop record into takes, take lanes and swipe comping
  (Quick Swipe, take folders), consolidate. Strip silence, clip groups and
  clip loops.

Exit criteria (the old Phase 3 criterion): an engineer who works in Pro Tools
comps a vocal without reading documentation.

## DAW 4 — Full mixer, processing graph and automation

- The parallel processing graph from `brain/specs/plugin-latency-compensation.md`,
  nodes dropped on deadline.
- I/O setup, aux tracks and buses, master, inserts, sends pre and post,
  sidechain, VCA, mix and edit groups, folder tracks and track stacks,
  loudness metering (BS.1770, R128, true peak), mixer views, bounce to disk.
- Automation: off, read, touch, latch, write, trim; lanes and editing;
  sample-accurate in the engine plan for faders, pans, sends and plugin
  parameters.

Exit criteria: a ctest on `drivers/fake` routes 18 tracks through 4 buses
with sends and a VCA, and the output matches a reference render sample for
sample. A touch pass and a latch pass written from the UI read back to
within one automation breakpoint of the gesture.

## DAW 5 — Plugin hosting and PDC

- The plugin ADR (M9), then `contracts/plugin`, `plugins/host`,
  `engine/plugin_client`. LV2 first, then VST3, then Windows VST through
  yabridge.
- Plugin delay compensation, plugin manager, presets and state in the
  session, editor windows (under WSLg during development; under the kiosk in
  OS 1), crash and liar test plugins.
- Freeze, bounce in place and commit; offline processing of a selection; a
  curated stock effect set (via plugins/content).
- The requirement, kept from the old Phase 4: every track playing with its
  plugin chain in real time, latency synchronised. At quantum 128 that is a
  2.67 ms budget for 18 tracks of plugins, so the graph is parallel from the
  start and plugins are dropped on deadline rather than waited on. PDC is
  cheap to design for and very expensive to retrofit.

Exit criteria: a track carrying 2048 samples of plugin latency lands
sample-aligned with a track carrying none (ctest). `kill -9` of the plugin
host mid-take leaves playback running and the take growing. 18 tracks with
four plugins each play on the laptop with zero xruns at the lowest quantum
the laptop holds without plugins; the figure at 128 on the UMC1820 is OS 1.

## DAW 6 — MIDI and virtual instruments

New on 2026-10-13: MIDI was absent from the roadmap.

- MIDI ports in `contracts/backend`, a MIDI spec and ADR before the model,
  MIDI regions in `common/model` and the session file.
- Recording (merge, replace, cycle takes), playback to instruments and
  external ports, instrument tracks hosting LV2 and VST3 instruments, a
  curated instrument set (via plugins/content).
- Piano roll, event list, step input, quantize and groove, MIDI operations,
  MIDI effects, MIDI learn, Standard MIDI Files, score and notation.

Exit criteria: a MIDI performance recorded against the click plays back
through a hosted instrument with every note within one sample of its
recorded position in the rendered output, and survives an SMF export and
re-import unchanged.

## DAW 7 — Tempo, time and pitch

Tempo editing and the tempo track, beat detection and tempo from audio,
Elastic Audio / Flex Time, pitch editing, varispeed, offline time and pitch
processing, groove track, arrangement and global tracks, sample-level
editing. Stretching uses a free library (Rubber Band class), never a
reconstruction of Avid's or Apple's algorithms.

Exit criteria: a drum loop recorded at one tempo conforms to a tempo map
with its transients within 1 ms of the grid, and a pitch edit renders with
the edited notes' fundamental within 5 cents of the target.

## DAW 8 — Interchange, import and export

RF64; export formats with dither and sample-rate conversion; stem export;
import from another Sonora session; `sonora-convert` (M10); AAF import and
export through `libaaf`; OMF import (whether `libaaf` reads OMF is
unverified); `.ptx` read-only through `libptformat`.

Exit criteria (the old Phase 4 criterion): a session round-trips to another
DAW through AAF with edits and automation intact.

## DAW 9 — Logic creative tools and content

Smart Controls, chord track, articulation sets, Step Sequencer, Live Loops.
Through plugins/content: a loop browser over user-owned and free-licensed
loops, sampler instruments in SFZ, Alchemy-class synths, Drummer and Session
Players class performers, stem separation and mastering-assistant class
tools.

Exit criteria: a Logic user builds a 16-bar pattern in the Step Sequencer and
launches four Live Loops scenes in time, without reading documentation.

## DAW 10 — Control surfaces and sync

HUI and Mackie Control (codecs against documented behaviour, ADR-0007 §10),
MTC, LTC, MIDI clock and MMC, word clock and ADAT (validated on the UMC1820
in OS 1). The video track and video timecode formats moved after launch
(ADR-0025). The direct ALSA backend, if it is ever needed. Key map coverage against both vendors'
guides for every command that now exists.

Exit criteria: the engine chases incoming LTC and MTC to within one frame.

## After launch (optional, ADR-0025) — Video, surround and immersive

A video track locked to the timeline and video timecode formats (a video
frame shown at a given timecode is the frame at that timecode, checked on a
burnt-in reference).

Multichannel buses, 5.1 and 7.1 panning, binaural panning (via plugins),
object mixing and ADM BWF export (ITU-R BS.2076). Dolby's renderer needs
Dolby's licence and is not assumed.

Exit criteria: a 7.1 session exports as ADM BWF that a reference reader
places in the right channels.

---

# Milestone 2 — OS packaging

Starts when the Complete DAW milestone has closed. Content kept from old
Phases 1, 5 and 6 and ADR-0010.

## OS 1 — Bootable image and target hardware

Already built and verified in containers and QEMU (kept): `live-build`
hybrid ISO, RT kernel as the only and default kernel, autostart into the
engine, boot menus that time out, `tools/write-usb.sh` with a persistence
partition.

Remaining:
- PipeWire quantum and rate actually pinned, RT limits.
- Boot with no login into the engine and the Qt UI fullscreen under the
  labwc kiosk (ADR-0010, accepted), no desktop visible; plugin editors under
  it; tty1 console chords.
- Automatic detection of a class-compliant USB interface, udev rule, the
  virtual-card QEMU test.
- Physical stick on physical machines, BIOS and UEFI; a verified way to
  write the stick from Windows; boot and USB tests in CI.
- **The UMC1820 is connected here.** All 18 channels, lowest zero-xrun
  quantum, ADR-0007 IPC figures, ADR-0009's hardware measurements, the
  plugin benchmark at quantum 128.

Exit criteria (the old Phase 1 criterion): a USB stick boots on two different
machines and records 8 tracks for 10 minutes with zero xruns at 128 frames.
The resulting files open in another DAW at the right timecode. Separately:
18 tracks with four plugins each play at quantum 128 with zero xruns on the
UMC1820.

## OS 2 — Hardening and updates

Read-only root with an overlay, A/B updates, recovery of a session after the
power is pulled mid-take.

Exit criteria (from the old Phase 5): 24 hours of continuous recording with
zero xruns, and a session recovered intact after the power is pulled
mid-take.

## OS 3 — Installable to disk

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
  OS 2 does for the image.
- UEFI entry and Secure Boot. Unsigned is tolerable for a stick and awkward
  for an installed machine.

`--debian-installer none` is set in `iso/auto/config` today. Whether the
installer is Debian's, Calamares, or something small of our own is an ADR, not
a decision to drift into. An appliance installer has one job and should not
offer a desktop-install decision tree.

Exit criteria: install to an internal disk, boot from it with the live medium
removed, and record a session that survives a reboot.
