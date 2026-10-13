# Backlog

Ordered within each phase. An item is ready when someone could start it
without asking a question.

## Phase 1

- [ ] Compile the engine on Debian trixie and fix what the compiler finds.
- [ ] Run `ctest`; make the ring buffer and BWF tests pass.
- [ ] `live-build` config: package list, RT kernel, PipeWire from backports.
- [ ] `sonora-session.service`: autologin on tty1 into the engine, respawn on
      exit, no display manager.
- [ ] PipeWire drop-in pinning quantum to 128 and rate to 48000.
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
- [x] QEMU boot smoke test. 19 checks: a running engine that holds its PID,
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

## Blocked on hardware the project does not have

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
- [ ] Measure before M8 (ADR-0009 Unverified 5): worst-case `process()` on
      the target machine with the UMC1820, 18 tracks × 2 voices plus 18
      inputs at quantum 128, pool and plans pre-faulted. rt-engineer.
- [ ] Measure before M8 (ADR-0009 Unverified 7): undo-to-audible latency,
      p99 and max, on a real stick while 18 tracks record. rt-engineer.

## ADR-0007 follow-ups

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
- [ ] With M3, `drivers/fake` gains two injections from the measurements:
      ports named without `AUX` (`capture_1..N`, what `snd-aloop` gives
      without `audio.position`), and registry ids out of channel order. The
      engine must still connect them in channel order. driver-engineer.
- [ ] Plugin editors under the kiosk compositor: try Qt on `xcb` as the
      transient parent, a host anchor window, and an alternative compositor,
      with a real LV2/VST3 editor. Check whether upstream cage still
      misplaces override-redirect popups. Needed before the Phase 4 plugin
      ADR and the compositor ADR. researcher, then architect.
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

## Phase 2

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
      exit-criterion ctest needs `drivers/fake` with captured outputs, so it
      waits for ADR-0007 M3. rt-engineer.
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
  Until RF64 exists the writer should refuse or split a take at the limit:
  `close()` casts both sizes with `static_cast<uint32_t>`
  (`common/audiofile/src/wav_writer.cpp:144`, `:147`), so today a longer take
  gets a wrapped header and reads back short or garbled, with no error. In
  the same file, `close()` never writes the RIFF pad byte after an odd-sized
  `data` chunk. That is unreachable while periods are even (mono 24-bit, odd
  frame count), and it is free to fix alongside.
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
