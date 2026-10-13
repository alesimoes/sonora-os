# Status

The single place to learn where the work stands. Any agent updates this at the
end of its run. Keep it short: what is true now, not a diary.

- **Updated:** 2026-10-13
- **Current focus:** the **Complete DAW** milestone, stage DAW 1 (engine
  core, contracts, model). Owner, 2026-10-13: the whole product first, at
  parity with the workflow Avid and Apple document for Pro Tools and Logic,
  then OS packaging (`planning/ROADMAP.md`). Critical path: M2 → M3 fake
  backend → playback through the monitor bus → record while rolling with
  in-band take boundaries → M4 control API → Qt UI wired to M4. Model in
  flight on `feature/timeline-model`. Development on the owner's laptop
  under WSL2 with onboard audio; no ISO, QEMU, USB or installer work until
  the product exists; the UMC1820 is connected at the end. The image work
  below is done and parked, not abandoned.
- **Build state:** the engine compiles clean under GCC 14 with `-Wall
  -Wextra` and all 17 ctests pass in the Debian trixie container built by
  `tools/build-engine.sh`. Verified 2026-10-13 on `contract/backend`, and
  under the CI sanitizer flags (TSan; ASan+UBSan) in the same container
  -- TSan needs `--security-opt seccomp=unconfined` and `setarch -R` there,
  because Docker Desktop's kernel randomises more address bits than TSan
  maps. The writer/disk-thread race TSan found is fixed in `a141e9d`.
- **ADR-0007 M1 landed (2026-10-12, `aa4fbec`, PR #5).** The C++ is in the
  module layout of ADR-0007 §1 (`common/`, `contracts/api`, `engine/core`,
  `engine/control`, `engine/app`, `drivers/jack`, `ui/keymap`, `ui/tui`)
  under one CMake root; `build/sonora-engine` is where it was. Three
  forbidden edges remain until M2 and M4, each commented where it is
  linked. Landed with it: a take's number comes after any track's files on
  disk, not only the tracks open now, and `WavWriter::open` flushes the
  header, so a disk that refuses it fails the take at start with the
  reason instead of in the disk thread later.
- **ADR-0007 M2, the backend contract (`contract/backend`).** JACK only in
  `drivers/jack/src`; `contracts/backend` holds `AudioBackend`,
  `ProcessCallback`, `AudioBlock`; `drivers/support` the shared helpers.
  Unplug, stall, rate and buffer changes are fatal and close the take; the
  engine locks its memory. Device policy, revised by the owner 2026-10-13
  (ADR-0007 M2 note item 8): `--device`, else `~/.config/sonora/device`,
  else `/etc/sonora/device`; else the one USB interface; else, once the
  `--wait-for-inputs` window is over with nothing connected, the one
  onboard device, with a notice kept on screen; several non-USB devices
  fail listing them; a USB interface plugged in later is noticed, not
  switched to. `--tracks auto` sizes to the chosen device (2 on onboard).
  Also from the final review (items 9 to 12): a misspelled device fails
  after the wait; a take with no input on an armed track is refused, saying
  whether some inputs are connected; teardown is bounded to 5 s; clock
  steps are shown beside xruns. All of it is container-verified only,
  including DeviceGone on a killed patched device: no UMC1820 has been
  attached, so the USB rule (aliases -> `/proc/asound/<card>/usbid`) is
  unproven on hardware, and the onboard fallback is unproven on the
  owner's laptop.
- **ADR-0007 M3, `drivers/fake` and the conformance suite
  (`feature/fake-backend`).** `FakeBackend` (platform-neutral) runs
  `process()` from `run_blocks()` on the caller's thread or on its own
  thread; UMC1820 (18/20, USB) and onboard (2/2) shapes; tones matching
  `verify-recording.py`, silence, impulses; captured outputs; injections
  for port naming without `AUX`, lexicographic listing, ids out of order,
  late enumeration, xrun, clock step, rate, quantum, unplug, stall, server
  loss. `contracts/backend/conformance` checks the contract's guarantees;
  it passes on six fake rigs in ctest and against JackBackend on PipeWire
  in `tools/test-engine-docker.sh`: 10 ok, 4 skipped. Xrun and clock step
  cannot be caused there, and the quantum and rate changes are held by the
  shipped pin, which the rig confirms on the running driver. `sonora-engine --backend fake` records 18 verified
  tracks with no audio server. Container only; no hardware involved.
- **PR #16 follow-ups (on `contract/backend`).** A take that fails while
  recording (full disk, 4 GB limit) is shown at once in red on the terminal
  UI, from `Session::take_failure()`, until the next take records cleanly;
  quitting or SIGTERM during a failed take restores the terminal, lets
  `stop()` print the reason and exits 1.
- **Key map review fixes (2026-10-12, `feature/ui-foundation`):** a take on
  disk is never overwritten -- the take number comes from the session folder
  and the writer creates with O_EXCL; arming is refused during a take; SS3
  modifier sequences decode as one key; Ctrl+C stays live when a profile has
  no quit key; SIGHUP stops cleanly; a dead play/stop key is repaired to
  `transport.stop`, so Space never starts a take in the Pro Tools or Logic
  profiles; our own chords are `"origin": "sonora"`, apart from unverified
  vendor bindings. Each has a regression test. Not yet exercised on tty1 or
  in QEMU; the interactive paths (Ctrl+C fallback, SIGHUP) are unit-tested
  only.
- **ISO builds.** `iso/sonora.iso`, 867 MB, last built 2026-10-12. Inspected
  rather than assumed: the engine is present at mode 0755, the service is
  enabled, the `sonora` user is in `audio`, all three tuning files are
  installed, and both bootloaders default to the RT kernel, which is now the
  only kernel in the image.
- **The engine records.** `tools/test-engine-docker.sh` runs it against a real
  PipeWire in a container and verifies the files: 18 tracks -- the UMC1820
  count -- at 48 kHz, quantum 128, zero xruns, zero overruns, -9.0 dBFS which
  is precisely the RMS of the sine fed in, and each track carrying the tone it
  is supposed to carry rather than a neighbour's. Runs in seconds, so it is
  the loop for engine work. Passed again 2026-10-12 after the fixes below.
- **The image boots.** `tools/test-boot-qemu.sh`, 2026-10-11, rerun on the
  rebuilt ISO 2026-10-12: up in about 13
  seconds on `6.12.111+deb13-rt-amd64`, `sonora-engine` running, no failed
  units, 19 of 19 checks passing (20 of 20 on 2026-10-13, see below). Among them: `libjack.so.0` resolves to
  PipeWire's rather than jackd2's; the Pro Audio profile is applied, so ports
  arrive as `...pro-input-0:capture_AUX0`; the engine holds the same PID across
  ten seconds with `NRestarts` at zero; there is no serial login prompt; and
  the operator is not in the `disk` group. The harness also asserts statically
  that both bootloader defaults load the RT kernel, comparing bytes rather
  than names.
- **The USB writer works, on a disk image.** `tools/test-write-usb.sh`,
  2026-10-12, runs the real `tools/write-usb.sh` in a privileged container
  against a loop device over an 8 GB sparse file. It refuses the container's
  root disk and every other Docker VM disk before the prompt. It writes over
  a used GPT stick and over a SonoraOS stick. Each time, the first 909,115,392
  bytes match `iso/sonora.iso` except the 11 bytes of MBR slot 3, and
  partition 3 is ext4, labelled `persistence`, holding `/home union`. QEMU
  then boots that image as a USB stick. A marker written into
  `/home/sonora` is back after a reboot. The smoke test passes from the
  stick, and the engine's session folder lands on the stick. Unattended
  boots through the stick's own isolinux (SeaBIOS) and GRUB (edk2) reach
  the engine. Before this, the script could not have worked: parted fails
  on the hybrid label after `dd` has already erased the stick, and if it had
  worked the script would have formatted the EFI partition. See
  `brain/postmortems/2026-10-12-write-usb-never-run.md`.
- **The boot menu now times out (5 s).** Until 2026-10-12 both bootloaders
  waited forever at their menu, so an unattended machine never reached the
  engine. Nothing caught it, because no test ran a bootloader unattended.
  ISO rebuilt 2026-10-12 with the fix.
- **Qt 6 shell (`ui/qt`, `sonora-ui`; #10).** 18 scene-graph meters,
  transport status, footer, track arming and key routing through `ui/keymap`,
  plus the Edit and Mix windows (#28), as a separate process talking to the
  engine through the control API (#33) — `FakeEngine` until M7's socket.
  `tools/build-ui.sh` builds it and runs 49 tests in the `sonora-ui` image
  (one renders frames under Xvfb); CI runs them in the `ui` job. The digit row is named by its physical key, so
  AZERTY and ABNT2 arm the right track (`keymaps/README.md`). Measured with
  llvmpipe only (`brain/research/qt-shell-render-cost.md`): no GPU figure.
  Open: no compositor or systemd unit for it in the ISO (a compositor ADR is
  architect's); whether `app.quit` closes only the UI; the real engine
  reaches Qt only after M7 (process split, socket + state block).
- **Control API landed (ADR-0007 M4, #33).** `contracts/api`: EngineControl
  (commands, replies/events, model ops with undo window and floor), a
  read-only state block read through StateView (consistent frames, stale
  flag, per-reader peak cursors), ≤64 KiB framing for large messages;
  `InProcessEngine` over the real Session and `FakeEngine` pass the same
  conformance suite (11/11). The terminal UI is a client of it; commands
  reach the audio thread only as atomic stores. Next: M7 process split.
- **The quantum and rate are pinned against clients (2026-10-13).** Under
  ADR-0007 M2 a block-size or rate change ends a take. The old drop-in let 11
  of 16 client routes move the graph (`jack_set_buffer_size`,
  `PIPEWIRE_QUANTUM`, `clock.force-*` metadata, `jack_set_sample_rate`). Now
  min = max = default = 128, `allowed-rates [ 48000 ]`, `quantum-floor`,
  `settings.check-*` and server-side `node.rules`; all 16 hold, measured in
  the runtime container by `tools/test-quantum-pin.sh` (new CI job). Two
  deliberate two-step `pw-metadata` edits still get through and are reported
  as gaps. Rebuilt ISO boots in QEMU with the running server reporting
  128/128/128 and `[ 48000 ]`: 20 of 20 smoke checks. Dummy driver only; no
  ALSA device has been the driver. `brain/research/quantum-pin.md`.
- **The USB writer writes only through descriptors, rechecked (2026-10-13).**
  No path-based write to the whole disk any more: the wipe (first and last
  MiB, perl, O_EXCL), dd, sfdisk and, for the partition, mkfs and mount all
  go through descriptors checked against sysfs and diskseq. Reopens through
  `/proc/self/fd/N` go by major:minor, so a device swapped mid-run is caught
  by the diskseq recheck after the step, not prevented; sfdisk runs with
  `--lock=no` so it cannot deadlock on our own flock. No O_CREAT open
  of the device; `flock -x` held during the write; a write-protected device
  is refused before the prompt; a missing or empty device is not reported as
  "in use". Each asserted in `tools/test-write-usb.sh` (strace and seeded
  patterns). Loop devices and QEMU only, as before.
- **Edit and Mix windows (`ui/qt`, branch `feature/ui-arrange-mixer`).**
  Pro Tools workflow over the synthetic engine: rulers (timecode from
  01:00:00:00, bars|beats, min:secs), scene-graph timeline with waveform
  overviews, Smart/Trim/Select/Grab, Slip and Grid, move/trim/separate/clear/
  nudge with undo, one strip per track plus the monitor bus. Edits go through
  `TimelineDocument` (`ui/qt/src/timeline_document.h`), a local stand-in
  shaped to `brain/specs/timeline-model.md`, swapped for `common/model` (on
  main) at M7.
  Every colour and size is a token from `brain/specs/ui-design.md`. Open:
  the audio is synthetic (no peak files), mix changes nothing audible (no
  playback path), Shuffle and Spot refuse edits, Inter is not yet in the ISO.
- **Still unverified:** no real audio interface has ever been attached, so
  there is still no latency or xrun figure from hardware. Nothing has run on a
  physical machine or a physical USB stick: every claim above is a loop
  device or a QEMU usb-storage device under SeaBIOS or edk2.
- A review of the initial commit already found three missing includes and a
  case where losing the audio server left the engine showing REC while
  capturing nothing (fixed in `dafebe6`). That is the expected yield from
  uncompiled code, and the reason CI is the next task rather than features.

## In flight

| Item | Owner | State |
|---|---|---|
| First compile and `ctest` run | distro-engineer | done, all three tests pass |
| `live-build` configuration | distro-engineer | builds; contents verified |
| Autostart into the engine | distro-engineer | boots and starts the engine in QEMU |
| Container recording test | rt-engineer | passing, wired into CI |
| QEMU boot smoke test | distro-engineer | passing, 20 checks |
| Quantum and rate pin against clients | distro-engineer | passing in container and QEMU; real interface untested |
| `write-usb.sh` against a disk image, five QEMU boots | distro-engineer | passing; physical stick untested |
| ADR-0007 M2, backend contract | rt-engineer + driver-engineer | PR from `contract/backend`; reviewed by both owners, `code-reviewer` and `rt-safety-auditor`; hardware untested |
| LV2 plugin host (`plugins/host`) and draft `contracts/plugin` | plugin-engineer | PR #26: 15 plugin ctest cases pass in `tools/build-plugins.sh` (depth 1 and poll-only); contract merged as a draft, acceptance items in BACKLOG Track 4; engine side is M4+ |

## Blocked

| Item | Blocked on |
|---|---|
| Hardware validation | No audio interface has been attached. The target is a Behringer UMC1820; every figure in this repository comes from a container or from QEMU. |

## Next decision needed from the owner

None open. Decided 2026-10-13: the Complete DAW comes before OS packaging,
and ADR-0007 §10's early-scope limit is lifted for it (dated amendment in
that ADR). Decided 2026-10-12: Phase 2 playback starts with a fixed
2-channel monitor bus; the full mixer comes later on the same graph
(`planning/ROADMAP.md`, Phase 2; `brain/specs/session-format.md`).
ADR-0009 (timeline snapshots, residency-gated plan swap) accepted by the
engine owner 2026-10-13, merged with the Phase 2 specs (#11).
