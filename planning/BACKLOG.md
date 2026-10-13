# Backlog

Ordered within each phase. An item is ready when someone could start it
without asking a question.

## Complete DAW (owner, 2026-10-13): critical path and parallel tracks

The product first, the OS after (`planning/ROADMAP.md`). Stages are DAW 1 to
DAW 11; the full feature list, with the Avid or Apple document each item's
behaviour comes from, is the "Complete DAW" front of `planning/progress.json`.
This section is the order. The older sections below keep their detail and
are cross-referenced, not repeated.

**Critical path:** M2 (`contract/backend`) → M3 fake backend → playback
through the monitor bus → record while rolling with in-band take boundaries
→ M4 control API with the §3 state block → Qt UI wired to M4 (DAW 2) →
parallel graph and full mixer (DAW 4) → `engine/plugin_client` with PDC
(DAW 5). The model (`common/json`, `common/model`) must land before
playback can follow edits; it is in flight.

### Track 1: engine (rt-engineer; driver-engineer for M3; auditor gates)

1. [ ] M2 backend contract, `contract/backend` (in review).
2. [x] M3 `drivers/fake` and the conformance suite, with the two port-order
       injections ("ADR-0007 follow-ups"). `feature/fake-backend`.
3. [ ] Playback: disk reader, chunk pool, residency-gated plan swap, output
       through the fixed 2-channel monitor bus (DAW 1 detail, below).
4. [ ] Record while rolling with in-band take boundaries; punch, loop,
       locate, pre-roll; input monitoring.
5. [ ] M4: control API, wire codec, state block and `StateView` per
       ADR-0007 §3, `InProcessEngine`, `FakeEngine`; removes the illegal
       edges; then M5, M6.
6. [ ] Take safety in DAW 1: 4 GB limit refused, periodic `fdatasync`, a
       take that survives `kill -9`, bext date.
7. [ ] M7 process split, during DAW 2.
8. [ ] Parallel processing graph, buses, sends, VCA, automation in the plan
       (DAW 4).

### Track 2: model (rt-engineer, ui-engineer review)

1. [ ] `common/json` and `common/model`, in flight on
       `feature/timeline-model`: snapshots, operations, undo, region
       operations.
2. [ ] ADR-0009 proxies before M8 merges: Unverified 3, 4, 6 as written; 5
       and 7 on the laptop, labelled proxies (ADR-0009 update 2026-10-13).
3. [ ] Session reader and writer; fades and crossfades; track types;
       templates, autosave and backups.
4. [ ] MIDI spec and ADR, then MIDI in the model (DAW 6).

### Track 3: UI (ui-engineer)

Against `common/model` and `FakeEngine` now; wired to M4 when it lands.

1. [ ] Arrange view: tracks, rulers, waveforms, regions; move, trim, split,
       edit commands, undo/redo; edit modes, Smart Tool, grid and nudge;
       zoom; fades; clip gain; markers.
2. [ ] Mixer view on the monitor bus: fader, pan, mute, solo, metering.
3. [ ] Transport window and counters; tempo map and click display.
4. [ ] Key maps driving M4 commands, profile switch, Commands Keyboard
       Focus; fix the `keymaps/default.json` comment and its embedded copy.
5. [ ] Playlists and comping (DAW 3), full mixer and automation UI (DAW 4).

### Track 4: plugins (plugin-engineer; rt-engineer for anything in `engine/`)

1. [x] Out-of-process LV2 host: scanning, validation, crash/hang/liar test
       plugins, the host loop, and the shared-memory round-trip measurement
       (PR #26; `brain/research/plugin-host-lv2-container.md`). The runtime
       canary check covers only 64 words past `max_frames`.
2. [ ] Accept `contracts/plugin/include/sonora/plugin/protocol.h`. Merged as
       a DRAFT with PR #26 (trust rule, private `Layout`, depth 1 default,
       no audio-thread wait); stays `kLayoutVersion` 0 until these land (the
       same list is the `TODO(accept)` block at the top of the header):
       - parameters: per-block-tagged changes with a frame offset, and a
         last-value table in the region;
       - crash/restart: never reuse a region a dead host touched; restart =
         new memfd + host + Ready + latency re-read, rejoining through a
         control-thread plan swap with parameters replayed;
       - plan swaps (ADR-0009): bypass atomic + RtEvent; `HostChannel` state
         keyed by stable node id outside the plan; an in-flight depth-1 block
         discarded across a chain swap; PDC across a `latency_generation`
         bump; a "dry" miss is dry delayed by `latency_frames`;
       - versioning: `header_bytes`, static_asserts on sizeof/offsetof, and
         how "incompatible" is reported;
       - the RT auditor's ruling on one `FUTEX_WAKE` per callback (depth 0
         poll-only depends on it too).
3. [ ] Plugin ADR (M9) from that measurement: depth 1 on the audio thread
       (the container saw a 2 ms timed wait return 5.9 ms late), host
       per chain/track/plugin, the wake syscall's auditor ruling, `HostControl`
       over the control socket (scan via one `--probe` per process,
       instantiate, remove, editor with the parent-window token).
4. [ ] `engine/plugin_client` and PDC, once M4 exists: `HostChannel::submit/collect`
       on the contract, hosts forked from the long-lived control thread and
       watched by pidfd (`PR_SET_PDEATHSIG` follows the forking thread),
       removal after N consecutive misses with the plugin seen running during
       the streak named to the operator, the per-plugin NaN/Inf counters
       shown; a
       2048-sample track lands aligned (ctest).
5. [ ] LV2 state save/restore, `worker:schedule`, presets; plugin manager;
       editors under WSLg (the host opens parentless X11 toplevels). VST3
       hosting is done (this PR, `brain/research/plugin-host-vst3.md`);
       VST3 state (`getState`/`setState`), instruments (event input, no
       audio input) and `hostchecker.vst3` against the host remain.
6. [ ] **Windows VST through yabridge** (next, plugin-engineer; not started).
       What it needs:
       - Wine and yabridge (GPL-3.0) in the plugins image and, later, the
         appliance. Debian trixie main has Wine 10.0 (`wine`, checked
         2026-10-13) but no yabridge: pin an upstream yabridge release, check
         which Wine versions it supports, and record both licences.
       - `yabridgectl sync` turns Windows `.vst3` bundles into Linux `.vst3`
         bundles under `~/.vst3/yabridge`, which the VST3 host already scans
         and runs. The Windows VST2 path makes `.so` VST2 plugins, which
         this host does not load: VST2 needs Steinberg's VST2 SDK licence
         (no longer offered) or a clean-room header; decide before doing it.
       - A test plugin with a Windows build under a free licence (an LSP or
         DPF Windows VST3), so CI runs one through Wine. Measure the cost:
         yabridge adds its own IPC hop (plugin host -> yabridge -> Wine),
         which lands inside this host's block budget and must be measured
         against the 2.67 ms deadline before it is offered.
       - Crash isolation holds as is: a Wine crash ends this host process.
       - Licence checks and copy protection inside Windows plugins (iLok and
         the like) are not circumvented (ADR-0004); a plugin that needs
         them under Wine either works or is reported unusable.
       - Editors stay out of scope (ADR-0010).
7. [ ] Freeze, bounce in place, offline processing; the curated stock set.

### Track 5: dev environment (distro-engineer, researcher)

1. [ ] Measure WSLg audio from WSL2 Debian trixie: can PipeWire bridge to
       WSLg's PulseAudio server for playback **and** capture, what quantum
       holds with zero xruns over 10 minutes, round-trip latency. Assumption
       until measured. researcher.
2. [ ] `tools/dev-wsl.sh` when engine and UI are first connected: builds
       engine and UI, starts PipeWire on onboard audio, runs both. Not
       before.

### Later stages, in order (detail in `progress.json`)

DAW 6 MIDI and instruments, DAW 7 tempo/time/pitch, DAW 8 interchange,
DAW 9 Logic creative tools and content, DAW 10 surfaces/sync/video, DAW 11
surround. Researcher, before DAW 6: pin the Pro Tools Reference Guide
edition (PT-RG) and check every `ref` in `progress.json` against it and
LP-UG 12.3; flag any feature that is content rather than behaviour.

## OS packaging (after the Complete DAW)

Every section from here to "ADR-0010 display stack" that touches the image,
the USB stick, QEMU or hardware is OS packaging (ROADMAP, Milestone 2). Do
not start or extend those items before the Complete DAW closes (owner,
2026-10-13). Engine items inside them (the bext date) are pulled into DAW 1.

## Phase 1 (OS 1, after the Complete DAW)

- [ ] Compile the engine on Debian trixie and fix what the compiler finds.
- [ ] Run `ctest`; make the ring buffer and BWF tests pass.
- [ ] `live-build` config: package list, RT kernel, PipeWire from backports.
- [ ] `sonora-session.service`: autologin on tty1 into the engine, respawn on
      exit, no display manager.
- [x] PipeWire drop-in pinning quantum to 128 and rate to 48000, and holding
      it against clients: `tools/test-quantum-pin.sh`, in CI.
- [ ] Repeat `tools/test-quantum-pin.sh`'s routes with the UMC1820 as the
      graph driver instead of the dummy driver. A real ALSA node may have
      period-size constraints the container cannot show.
- [ ] Decide whether to ship `default.clock.quantum-limit = 128`, which
      closes the measured `clock.max-quantum` + `clock.force-quantum` gap but
      also sizes the server's ALSA and resampler buffers. Needs a device that
      cannot run at 48 kHz to test against. `brain/research/quantum-pin.md`.
- [ ] `limits.d` granting `rtprio 95` and `memlock unlimited` to the audio
      group.
- [ ] Udev rule naming the first class-compliant USB interface so port
      auto-connection is stable across reboots.
- [x] `tools/write-usb.sh`: write the ISO and create the persistence partition.
      Verified against a loop device and booted in QEMU, with persistence
      surviving a reboot: `tools/test-write-usb.sh`. Not yet run on a
      physical stick.
- [ ] Write a physical USB stick with `tools/write-usb.sh`, boot it on a
      physical machine, BIOS and UEFI, and confirm a session survives a
      reboot. The first time the script's USB/removable check meets real
      hardware.
- [ ] Run `tools/test-write-usb.sh` in CI. The container half
      (`WRITE_USB_BOOT=0`) needs only Docker and the ISO; the boots need QEMU
      with KVM.
- [ ] A verified way to write the stick from Windows, where the owner works.
      The first attempt (`write-usb-windows.ps1`, removed 2026-10-12) opened
      `\.\PhysicalDrive` with one leading backslash, which .NET resolves to
      `C:\PhysicalDrive`; it erased the stick and then wrote nothing. Check any
      Windows device path byte for byte: a doubled backslash sent through the
      agent's shell tool can arrive as a single one.
- [ ] Boot test on real hardware; record the xrun count in
      `brain/research/`.
- [x] QEMU boot smoke test. 20 checks, including the live PipeWire quantum
      and rate: a running engine that holds its PID,
      `-rt-` in `uname -r`, no failed units, no serial login prompt, and one
      check per defect found so far. `tools/test-boot-qemu.sh`.
- [x] A way to watch it boot by hand: `tools/run-qemu.sh`, going through the
      real bootloader with the shipped command line.
- [ ] bext OriginationDate/OriginationTime: `WavWriter` leaves both zero
      (`common/audiofile/src/wav_writer.cpp:83-92`). EBU Tech 3285 v2 §2.3:
      10 ASCII characters, year-month-day, and 8 ASCII characters,
      hour-minute-second (separator `-` recommended). Every DAW shows a blank
      creation date. Fill them in `open()`, which runs on the control thread,
      not the audio thread, with a regression test in
      `common/audiofile/tests/test_wav_writer.cpp` first. Then delete the
      `xfail` branch in `tools/bwf-interop.py`, whose format checks already
      run once the fields are non-empty. Found by `tools/test-bwf-interop.sh`;
      see `brain/research/bwf-interop.md`. rt-engineer.
- [ ] Run the boot test in CI. Needs the ISO built first, which takes tens of
      minutes, so it belongs in a separate workflow on a schedule or manual
      dispatch rather than on every push.

## Blocked on hardware the project does not have (OS 1; the UMC1820 is connected at the end)

- [ ] Confirm the Pro Audio profile actually exposes every channel on the
      owner's Behringer UMC, and record the model and channel count in
      `brain/research/multichannel-usb-interfaces.md`.
- [ ] First real latency figure: lowest quantum that holds with zero xruns on
      that interface, on the RT kernel.
- [ ] ADR-0007 IPC figures on the RT kernel, bare metal, with the engine
      recording 18 channels at quantum 128: socket round trip, `StateView`
      read, futex versus eventfd wake. Include the cost of the wake syscall
      on the audio thread, and whether any xrun is attributable to a reader.
      Feeds the Phase 4 plugin ADR. See `brain/research/adr-0007-measurements.md`.
- [ ] OS 1, proxy on the laptop before M8 merges (ADR-0009 update
      2026-10-13). ADR-0009 Unverified 5: worst-case `process()` on
      the target machine with the UMC1820, 18 tracks × 2 voices plus 18
      inputs at quantum 128, pool and plans pre-faulted. rt-engineer.
- [ ] OS 1, proxy on the laptop before M8 merges. ADR-0009 Unverified 7:
      undo-to-audible latency,
      p99 and max, on a real stick while 18 tracks record. rt-engineer.

- [ ] On the owner's laptop, no interface: the engine records from the
      onboard card after the 60 s wait, the notice on tty1, `--tracks auto`
      gives its channel count (expected 2). Then with the UMC1820: it is
      chosen over onboard, and plugged in mid-session it is noticed and not
      switched to (ADR-0007 M2 note, item 8).

## ADR-0007 follow-ups

- [ ] A pty-driven check (as in `tools/bwf-interop.py drive`) that a quit
      during a failed take exits 1 with the terminal restored. Only the
      `Session::take_failure()` half is unit-tested today.

- [ ] RTSan CI job: `clang-22` from trixie main, its own build
      (`-fsanitize=realtime`, which cannot be combined with ASan or TSan),
      `-Wfunction-effects` advisory. `SONORA_RT` expands to
      `[[clang::nonblocking]]`. Most useful after M3 (fake backend). Also
      check whether libjack calls trip RTSan inside the callback.
      distro-engineer with rt-engineer.
- [ ] With M4, the state publisher is paced on a fixed timer. `StateView`
      makes a bounded number of retries and reports a stale flag. Add a
      starvation regression test, and a `static_assert` for every
      shared-memory atomic type. rt-engineer.
- [ ] Virtual-card QEMU test: `snd-dummy model=rme9652` (26 ordered AUX
      ports) through the real ALSA → PipeWire → JACK path, with the engine
      actually recording. The count is per node. driver-engineer with
      distro-engineer.
- [x] With M3, `drivers/fake` gains two injections from the measurements:
      ports named without `AUX` (`capture_1..N`, what `snd-aloop` gives
      without `audio.position`), and registry ids out of channel order. The
      engine must still connect them in channel order. driver-engineer.
- [ ] Plugin editors under the kiosk compositor. ADR-0010 (accepted) picks
      labwc with the UI on native Wayland, which retires the `xcb` and
      anchor-window variants. Remaining: a real Qt 6 UI and a real LV2/VST3
      X11 editor under ADR-0010's labwc config (placement, override-redirect
      popups, focus back to the UI, Xwayland lazy-start latency). Needed
      before the Phase 4 plugin ADR. researcher.

## ADR-0010 display stack (accepted by the owner 2026-10-13; OS 1, after the Complete DAW)

The M4, M5 and doc items at the end of this section are DAW 1 work and are
not deferred.

- [ ] Stage 1, shipped boot unchanged: package block (`libpam-systemd labwc
      xwayland qt6-wayland` + Qt runtime), `/usr/share/sonora/labwc/`,
      `/etc/pam.d/sonora-ui`, `sonora-compositor`, `sonora-wait-for-display`,
      compositor/UI/fallback units behind `sonora.gui`. QEMU checks: ISO size,
      time to first frame, `kill -9` UI and labwc recover, repeated UI failure
      ends on tty2, renderer chosen on `-vga std`, Alt-Tab/Super-Return reach
      the client. distro-engineer.
- [ ] Stage 1, UI side: `app_id` `sonora-ui`, no fullscreen request, a
      first-frame log line, a documented "unfit display" exit code; later
      `sd_notify` watchdog through `common/platform`. ui-engineer.
- [ ] Stage 2, after M7: `sonora-session.service` becomes
      `sonora-engine.service` + `sonora-tui.service` (tty2); compositor
      enabled by default; smoke test kills labwc and the UI mid-take and
      asserts the engine PID and a growing take file. distro-engineer.
- [ ] Bare metal: GPU reset/hang under the compositor vs the audio callback;
      nouveau on NVIDIA; llvmpipe vs GPU xruns (ADR-0006 items 2 and 3).
      researcher.
- [ ] M4: fix the key map comment "Matches what engine/src/main.cpp handles
      today" in `keymaps/default.json` **and** in the copy of that profile
      embedded in `ui/keymap/src/keymap.cpp`, in the same commit, once
      commands are handled in `engine/control`. ui-engineer.
- [ ] M4 before M5: remove the illegal edges `engine/app` → `ui/keymap`,
      `ui/tui` and `test_keymap` → `engine/control`, `ui/tui`. M5's check
      would flag them (ADR-0007, "lessons from M1", item 1). rt-engineer
      with ui-engineer.
- [ ] M5: add the §7 ctest labels, one per top-level module directory.
- [ ] With or right after the M1 merge, update docs that name pre-M1 paths
      (`engine/src`, `engine/include`, `engine/tests`):
      `brain/specs/plugin-latency-compensation.md`,
      `brain/research/multichannel-usb-interfaces.md`,
      `brain/research/adr-0007-measurements.md`,
      `brain/research/protools-logic-key-bindings.md`, and the agent
      charters in M6. (`keymaps/README.md` is already done on the M1 branch.) ADRs
      and postmortems keep the paths they were written with.

## ADR-0008 portability

None of these may delay a Phase 1 or Phase 2 item (owner, 2026-10-12).
Order and owners are in ADR-0008 §5.

- [ ] P1, after M2 merges and before M5: `engine/core/src/session.cpp` to
      `std::filesystem`, using only the non-throwing overloads. Add a test:
      a session path that is a file makes `start_recording` refuse.
      rt-engineer.
- [ ] P2, with P1: `test_takes` to `std::filesystem`. Move the full-disk
      case of `test_wav_writer` to `common/audiofile/tests/posix/`,
      registered `if(UNIX)`. rt-engineer.
- [ ] P3, after M2: an `engine/app/include/sonora/process_setup.h` seam,
      covering `lock_memory()`, `install_quit_handlers()` and
      `default_session_root()`, with the implementations under
      `engine/app/src/posix/`. rt-engineer.
- [ ] P4, inside M4: `test_keymap` loses termios (those checks move to a
      `ui/tui` test). It removes directories with `std::filesystem`.
      ui-engineer.
- [ ] P5, before or right after `feature/qt-shell` merges:
      `ui/qt/src/measure.cpp` uses `std::chrono` and `std::clock`.
      Per-thread CPU goes through a new `common/platform`
      `thread_cpu_times()`, with posix and unsupported implementations.
      ui-engineer and rt-engineer.
- [ ] P6, with M5: `SONORA_PLATFORM` (linux, neutral, and windows, which
      fails at configure for now) and `SONORA_BACKENDS`. Compiler flags go
      under generator expressions. Add the neutral-module include allowlist,
      a platform-macro check and an OS-path check to M5's script.
      distro-engineer.
- [ ] P7, right after M5 and before M6: a required `portable` CI job on
      `windows-latest` with MSVC, building `SONORA_PLATFORM=neutral` and
      running `ctest`. Record the first 10 run durations in `brain/research/`.
      distro-engineer.
- [ ] P8, inside M6: put the ADR-0008 §1 table into OPERATIONS.md and the
      charters, and propose the rule for CLAUDE.md. The CLAUDE.md edit is
      the owner's.
- [ ] P9, inside M7: socket and shared memory behind `common/platform`
      (`ControlChannel`, `SharedRegion`). No socket or mapping code in
      `engine/control` or `ui/client`. rt-engineer and ui-engineer.
- [ ] P10, once `ui/qt` is on main: Qt in the `portable` job, from a cached
      install. distro-engineer.
- [ ] Only when the owner opens a Windows ADR: read the ASIO SDK licence
      from Steinberg's current distribution (it is reported as dual
      proprietary/GPLv3 since 2025-10-15, and that is unverified), and
      record it in `brain/research/` with the GPL-3.0-or-later analysis.
      researcher.

## DAW 1 detail (formerly Phase 2)

- [ ] Playback path: read from disk into a ring, mix, feed the output ports.
- [ ] Input monitoring with latency compensation measured by a loopback test.
- [ ] Transport locate without dropping a block.
- [ ] Timeline data model: regions over sources, with an undo stack.
- [ ] Session file format spec, then the reader and writer. Specs written
      (`brain/specs/session-format.md`, `brain/specs/timeline-model.md`,
      ADR-0009 accepted 2026-10-13); reader and writer not started.
- [ ] `common/json`: strict JSON with integer-exact numbers, line and key
      path in errors, size and depth caps (ADR-0009 §5). `common/model`
      uses it; `ui/keymap` moves onto it in its own PR. rt-engineer, with
      ui-engineer review.
- [ ] Bug: `ui/keymap`'s JSON parser reads numbers with `std::strtod`
      (`ui/keymap/src/keymap.cpp`, `number()`), which honours the C locale.
      Under `de_DE` the decimal separator is `,`, so `0.5` parses as `0`.
      Fix with `std::from_chars`; regression test that sets a comma locale
      first. ui-engineer.
- [ ] Close a take while rolling with **in-band boundary records**
      (`brain/specs/timeline-model.md`, "Closing a take while rolling"):
      audio thread → SPSC ring → disk thread splits at exactly the frame;
      the capture ring is never reset; the next take's files opened ahead;
      the disk thread reports "take closed, length" before `RecordTake`.
      Capture start/stop and the auto-monitor switch are sample-accurate
      inside a block, with a ramp at A and B. This **replaces the take-epoch
      handshake** in "Known gaps": with no ring reset there is no race. The
      race's regression test is still written first. rt-engineer, audited
      by rt-safety-auditor.
- [ ] Periodic `fdatasync` of every open take file from the disk thread,
      through `common/platform`, with the loss window stated in the UI and
      in `brain/specs/session-format.md` ("Crash recovery"). Measure its cost
      on a stick at 18 tracks first; it must not cause an overrun.
      rt-engineer.
- [ ] Disk reader thread and source chunk pool with per-plan chunk-slot
      arrays; plans and locates published only once their window is
      resident; one pending plan at most; exhaustion never evicts the
      playing plan (ADR-0009 §4). Comes with the ThreadSanitizer stress test
      of the retirement protocol (§4.1). rt-engineer, gated by
      rt-safety-auditor.
- [ ] Playback to `AudioBlock::outputs` through the monitor bus. The
      exit-criterion ctest needs `drivers/fake` with captured outputs
      (`FakeBackend::read_output()`, from M3). rt-engineer.
- [ ] Measure, before the reader is written: mixed record+playback
      throughput and worst-case read latency on a physical stick's
      persistence partition (18 in, 18 out, 8 k and 16 k frame chunks,
      coalesced reads). researcher; ADR-0009 Unverified 1.
- [ ] Measure before M8, no hardware needed: pool demand of a 2,000-region
      comp at the placeholder chunk size (ADR-0009 Unverified 6); undo
      memory for single and bulk edits on 18 × 2,000 regions against the
      64 MiB budget (Unverified 3). rt-engineer. The hardware ones are under
      "Blocked on hardware".
- [ ] Measure: `rename` plus directory `fsync` durability through the
      persistence overlay, 200 killed QEMU saves. researcher; ADR-0009
      Unverified 2.

## Known gaps, not yet scheduled

- RT-path data race, documented in `engine/core/src/session.cpp` above the
  ring reset in `start_recording()`: an audio callback that loaded Recording
  for the previous take and was descheduled across `stop()` and the next
  record command can write to the ring while it is being reset, putting the
  old take's samples at the head of the new file. Needs an RT stall of about
  100 ms to hit. Fix with a take-epoch handshake with the callback; write the
  regression test first. rt-engineer, audited by rt-safety-auditor.
  **Superseded by ADR-0009:** the fix is now the in-band boundary records
  in Phase 2 ("Close a take while rolling"), which remove the ring reset.

- RF64 for files over 4 GB. The 32-bit RIFF size field overflows at about
  8 hours of 24-bit mono at 48 kHz. See `brain/specs/bwf-broadcast-wave.md`.
  Until then `WavWriter` stops at the limit instead of wrapping: the file
  ends at the last whole frame that fits, and the take is reported failed
  ("take reached the 4 GB WAV limit") when it stops.
- Session origin. Every session's first take has TimeReference 0, because
  the playhead starts at 0. Ardour 8 treats 0 as "no timestamp" and imports
  it at the edit point (`gtk2_ardour/editor_audio_import.cc:989`, tag 8.12).
  A non-zero session start would avoid that, but it is a timeline decision
  (ADR), not a writer fix. `brain/research/bwf-interop.md`, finding 2.
- `DiskThread` polls every 10 ms. Correct, but a condition variable signalled
  by the callback would be better once there is a reason to care. It must not
  make the callback block.
- The disk thread writes with buffered `stdio`. Direct I/O with aligned buffers
  avoids double-buffering through the page cache; worth measuring before
  changing.
- No RT priority is requested for the audio thread yet; PipeWire sets it for
  its own client thread, which is enough until we own threads.
- `KeyMap::terminal_chords()` assumes xterm's modifier encoding. On the Linux
  console (tty1, where the appliance runs) Alt+Fn and Alt+Left/Right switch
  virtual terminals, Shift+PgUp/PgDn scroll the console, and Ctrl/Shift+arrow
  send no modifier parameter at all, so those chords never reach the engine
  but are counted as sendable by `ensure_essentials()` and listed in the
  footer. No shipped essential depends on one today; a profile that put its
  only arm, record, stop or quit key there would pass the check and not work.
  Fix by deriving the device set from the console's keymap (`dumpkeys`), or
  by a tty1-specific table.

## ADR-0011 native plugin suite (proposed 2026-10-13)

Stages N0–N7 are ADR-0011 §6. Nothing here goes into `engine/` except the
meters (N1), which the rt-engineer writes. Owner rulings that gate items are
ADR-0011 "For the owner".

- [ ] N0: `common/dsp` skeleton (neutral, `arch/generic` + `arch/x86_64`
      for flush-to-zero), `plugins/native/sdk` (`Processor`, parameter table,
      descriptor and preset codecs on `common/json`), and the new edges in
      M5's check. dsp-engineer.
- [ ] N0: `sonora-dsp-measure` harness and golden-file infrastructure:
      response, THD, step timing, aliasing, latency equals reported,
      denormal, non-finite, zipper; RTSan job runs it. dsp-engineer.
- [ ] N0: BS.1770 loudness and true-peak in `common/dsp`, checked against
      EBU Tech 3341/3342 cases (redistribution of the test files to be
      checked first). dsp-engineer.
- [ ] N0: EQ, gate, compressor, limiter, delay, chorus as `Processor`s,
      meeting their `quality.json`. CPU figures on the owner's laptop
      recorded in `brain/research/` (ADR-0011 Unverified 4). dsp-engineer.
- [ ] Research, before N1: licences of DPF (pinned commit), the VST3 SDK
      (MIT since 3.8, reported), ONNX Runtime; ONNX Runtime on trixie and
      its installed size (ADR-0011 Unverified 1, 3). researcher; feeds
      `brain/research/native-plugin-suite.md`.
- [ ] N1, after `feature/plugin-host-lv2` merges: `plugins/native/format`
      on DPF, LV2 and VST3; `lv2lint` and Steinberg `validator` in CI; the
      DPF-VST3 checkpoint on the EQ (Unverified 2). dsp-engineer,
      plugin-engineer reviews.
- [ ] N1, with M9: plugin-parameter target on `ParameterChange` and the
      read-only telemetry region in `contracts/plugin`. plugin-engineer +
      rt-engineer + ui-engineer.
- [ ] N1: descriptor-driven Qt editor panels in `sonora-ui`. ui-engineer.
- [ ] N1: engine track and master meters on `common/dsp` (peak, loudness,
      true-peak), gated by rt-safety-auditor. rt-engineer.
- [ ] N2: ngspice in the build container; SPICE reference circuits for the
      opto, FET, tube and transformer models (Unverified 8). dsp-engineer.
- [ ] N2: FDN reverb, tape, tube, transformer, opto, FET, mix bus as
      physical models, modelled noise and hum (seeded, switchable,
      visible). The tuner's DSP with key/scale parameters. dsp-engineer.
- [ ] Before the first capture session: class-E cost of NAM-class
      architectures plus a 200 ms cabinet IR at q128 (Unverified 9).
      dsp-engineer.
- [ ] N3: `common/dsp/neural` kernels, `.nam` reader, partitioned
      convolution, amp and cab processors; equivalence test against the NAM
      MIT reference implementation; first captures and IRs of owned or
      rented hardware with capture cards. dsp-engineer; hardware is the
      owner's (item 6).
- [ ] N4: mastering console as one processor; `sonora-analyze` with a
      rule-based mastering-suggestion pass. dsp-engineer.
- [ ] Research, before N5: a source-separation model whose weights we may
      ship (Unverified 10); accuracy of reverb/delay estimation from a
      reference (Unverified 11). researcher.
- [ ] N5: mix assistant plan machinery (plans as `common/model`
      operations, diff preview, one compound undo); first feature
      tempo-synced reverb and delay with reference estimation and the
      excess check; then gain staging, buses, parallel chains, mix bus.
      dsp-engineer with ui-engineer.
- [ ] N6: ONNX Runtime in `sonora-analyze`, model cards and manifest; key
      detection, timbre matching, artist-tone matching, tuner following the
      session key. dsp-engineer; researcher clears each card.
- [ ] N7, only after the owner's rulings (item 8): `sonora-assistant`, the
      optional Claude layer over analysis features and session state, never
      audio; graceful fallback to native suggestions. ui-engineer.
