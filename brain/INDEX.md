# Brain index

One line per entry. Agents read this before opening anything else.

## Decisions
- [ADR-0001](decisions/ADR-0001-engine-from-scratch.md) — engine written from scratch, not a fork of Ardour.
- [ADR-0002](decisions/ADR-0002-debian-base.md) — Debian stable as the distribution base.
- [ADR-0003](decisions/ADR-0003-pipewire-jack-api.md) — PipeWire as the server, JACK API as our client interface.
- [ADR-0004](decisions/ADR-0004-no-proprietary-reverse-engineering.md) — workflow cloning is in scope, binary reverse engineering is not.
- [ADR-0005](decisions/ADR-0005-usb-boot-persistence.md) — hybrid ISO plus a persistence partition for projects.
- [ADR-0006](decisions/ADR-0006-qt6-ui-toolkit.md) — Qt 6 for the shell, custom GPU scene-graph items for meters and timeline; the UI pulls at 60 Hz, the audio thread never pushes.
- [ADR-0007](decisions/ADR-0007-modules-contracts-and-squads.md) — modules by owner (`contracts/`, `common/`, `engine/`, `drivers/`, `ui/`, `plugins/`, `interchange/`), a dependency DAG enforced in CI, the RT boundary by module, contract change rules, squad mechanics and a six-step migration; engine and UI as separate processes, and Phase 3/4 work opened early under a fixed scope (both owner decisions, 2026-10-12). Updated with lessons from M1: three illegal edges until M4, so M4 merges before M5; tests live with their subject module; ctest labels go in M5; M2 is a `contract/backend` PR with `noexcept SONORA_RT`. Owner amendment 2026-10-13: §10's early Phase 3/4 scope limit lifted for the Complete DAW milestone.
- [ADR-0008](decisions/ADR-0008-portability.md) — portability (owner, 2026-10-12): neutral modules are standard C++17 only, platform code sits in `drivers/*`, `engine/app`, `ui/tui` and `common/platform`; `SONORA_PLATFORM` selects linux/neutral/windows(reserved); a required MSVC `windows-latest` job builds and tests the neutral modules; Windows backend will be ASIO, its licence unverified; Linux first.
- [ADR-0009](decisions/ADR-0009-timeline-snapshots-and-plan-swap.md) — accepted (engine owner, 2026-10-13): the edit state is an immutable value and undo a pointer, bounded by bytes with an engine-sent undo floor; resolved operations through a pure `apply` are both undo and UI replication; sources append-only and mix outside undo; playlists never overlap; a plan is published only once its audio is resident in per-plan chunk slots, retired by a seq_cst block counter; takes close in-band while rolling; canonical strict JSON through a new `common/json`. Update 2026-10-13: hardware measurements (Unverified 1, 2, 5, 7, 8) move to OS 1; laptop proxies before M8 merges.

- [ADR-0010](decisions/ADR-0010-kiosk-compositor.md) — **Accepted by the owner 2026-10-13.** labwc as a configured kiosk (no keybinds, UI always-on-bottom, editors floating via lazy Xwayland), Qt on native Wayland, logind via `libpam-systemd`; engine, compositor, UI and tty2 TUI in independent units with an `OnFailure` fallback to tty2. sway is the fallback if labwc fails on real hardware; cage, weston kiosk-shell and EGLFS (not in Debian's Qt) rejected.

## Specs
- [Plugin latency compensation](specs/plugin-latency-compensation.md) — real-time plugin playback, PDC, and why the graph must be parallel from the start.
- [BWF](specs/bwf-broadcast-wave.md) — the `bext` chunk layout we write, field by field.
- [Session format](specs/session-format.md) — the native `.sonora` file: canonical JSON, integer samples, sources/regions/crossfades/playlists, fixed stereo monitor bus (owner, 2026-10-12), atomic save, versioning, BWF mapping, crash recovery, AAF constraints.
- [Timeline model](specs/timeline-model.md) — `common/model`: snapshots, operations, undo, punch-in as the exit criterion, the playback plan and the monitor bus graph, residency-gated plan swap.

## Research
- [Multichannel USB interfaces](research/multichannel-usb-interfaces.md) — Pro Audio profile, WirePlumber state hooks, port ordering; what is verified and what is not.
- [ADR-0007 measurements](research/adr-0007-measurements.md): the six "Unverified" items. RTSan is available (`clang-22` in trixie main; the default clang 19 lacks it). 8-byte atomics are lock-free on amd64 and arm64. Container IPC figures: socket and seqlock costs are fine, but a seqlock publisher must be paced and spinning at FIFO hits RT throttling. `snd-dummy model=rme9652` gives 26 ordered AUX ports in QEMU. cage maximizes parentless X11 editors.
- [Pro Tools and Logic key bindings](research/protools-logic-key-bindings.md) — every binding in both profiles checked against Avid's and Apple's own guides; corrections, collisions, the Commands Keyboard Focus problem, and what the schema cannot express.
- [Qt shell render cost](research/qt-shell-render-cost.md) — 18 meters at 60 Hz on llvmpipe only: about one core, 0.04 ms GUI tick; no GPU figure yet; the software scene graph cannot draw the meters at all.
- [USB stick layout](research/usb-stick-layout.md) — the ISO's MBR already has the ESP as partition 2, so persistence is 3; parted cannot edit it; the overlay lives in `home/rw`; `systemd.run=` needs `systemd.unit=` on this image.
- [BWF interop](research/bwf-interop.md) — takes checked with ffprobe, soxi, libsndfile, mediainfo and headless Ardour 8.12 (`tools/test-bwf-interop.sh`, CI job `interop`). All 36 files open as 24-bit 48 kHz at the right length, and Ardour places each at its TimeReference, including a non-zero take 2. Defect: OriginationDate/Time empty. Ardour treats TimeReference 0 as no timestamp. The writer wraps sizes past 4 GB instead of stopping.

## Postmortems
- [2026-10-11 comment in a line continuation](postmortems/2026-10-11-comment-in-line-continuation.md) — dropped half a QEMU command line; bash -n cannot catch it. Also: an edit that reported success without landing.
- [2026-10-11 ring buffer data race](postmortems/2026-10-11-ringbuffer-data-race.md) — producer used the consumer's memory ordering; every test passed, TSan did not.
- [2026-10-11 RT kernel not default](postmortems/2026-10-11-rt-kernel-not-default.md) — the image installed the RT kernel but booted the generic one; no symptom except unexplained xruns.
- [2026-10-12 write-usb.sh never run](postmortems/2026-10-12-write-usb-never-run.md) — would have erased the stick and stopped, or formatted the EFI partition; its guard offered to erase a Docker VM disk.
- [2026-10-12 boot menu waits forever](postmortems/2026-10-12-boot-menu-waits-forever.md) — live-build's isolinux and GRUB templates have no timeout; no test ran a bootloader unattended.
- [2026-10-11 debootstrap tar failure](postmortems/2026-10-11-livebuild-tar-failure.md) — live-build cannot build a chroot on a Windows bind mount; also caught a masked exit code and a missing engine install.
