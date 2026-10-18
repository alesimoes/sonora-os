# Status

The single place to learn where the work stands. Any agent updates this at the
end of its run. Keep it short: what is true now, not a diary.

- **Updated:** 2026-10-13 (PRs #41-#47, and engine/plugin_client after the PR #49 reviews)
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
- **Transport window and the tempo map (`feature/ui-transport`).** Bars and
  beats are read through the whole tempo map (they were read from its first
  entry, so a loaded session with a tempo change had a counter, ruler and
  grid that disagreed with it after the change): counters, the Bars|Beats
  ruler, the Edit window's grid lines and Grid-mode snapping. A meter change
  starts a bar where it stands; a tempo change carries the count on. The
  Transport window (`window.transport`, Pro Tools' Ctrl+numpad 1, and a
  button in the bar) floats over either window: the transport keys, the
  position in every format from one reading, tempo, meter and the next
  change, the map, pre/post-roll, loop and punch. Pre/post-roll and the
  click are shown dimmed with the reason: the engine runs neither yet. Open:
  no tempo or meter ruler, no tempo editing, no counter entry (no locate
  command), and the nudge still steps by the first entry's grid value.
- **Edit and Mix windows (`ui/qt`, branch `feature/ui-arrange-mixer`).**
  Pro Tools workflow over the synthetic engine: rulers (timecode from
  01:00:00:00, bars|beats, min:secs), scene-graph timeline with waveform
  overviews, Smart/Trim/Select/Grab, Slip and Grid, move/trim/separate/clear/
  nudge with undo, one strip per track plus the monitor bus. Edits go through
  `TimelineDocument` (`ui/qt/src/timeline_document.h`) into `common/model`
  itself: its apply(), its byte-bounded History with the floor, ids from
  next_id. Trimming into a neighbour overwrites it, as Slip does in Pro
  Tools, and the drag previews the overwrite. File > Save / Open write and
  read the native session file (common/model's codec) under `~/Sessions`,
  from the interface for now: atomic and synced, but no rotated backups and
  no autosave until the engine's saver exists. At M7 the document becomes a
  replica of the engine's (`model::Replica`, PR #33).
  Every colour and size is a token from `brain/specs/ui-design.md`. Open:
  the waveforms are synthetic (no peak files), Shuffle and Spot refuse
  edits, Open has no key yet (no `app.open` in the catalogue), Inter is not yet in
  the ISO.
  **Fades (`feature/ui-fades`):** Fade to Start, Fade to End and the
  no-dialog crossfade on the profiles' keys (Pro Tools d/g/f, Logic
  ctrl+alt+x), the Smart Tool's fade corners with a live preview, Clear on a
  fade's span; one `SetFade`/`SetCrossfade` each, which the engine already
  plays. Curves are drawn by the engine's own `fade_in_gain` (moved to
  common/base `sonora/fade_curve.h`, so the two cannot drift), and a
  crossfaded end shows the crossfade, not the region's own fade the plan
  builder replaces. Fade room counts the fade a crossfade hides, as the model
  does; a crossfade over a region's own fade at its other end is refused.
  Open: no Fades dialog, no batch fades over several regions, crossfades
  only pre, centred or post (the model has no other alignment).
- **Playback landed (#41, ADR-0009 update).** The engine records and plays
  back through the fixed stereo monitor bus, driven over the M7 socket by
  `sonora-ui`, `sonora-tui` or `sonora-ctl`; `tools/run-sonora.sh` starts
  engine and UI as two processes.
- **Mix window on the monitor bus (`feature/ui-mixer-monitor`).** A fader
  ride is heard through ParameterChange per step and saved by one SetMix at
  release (the bus, which has no ParamId, rides through SetMonitorBus);
  gain and pan are typed into their readouts, with text off the travel
  refused, not clamped; Alt-click solo/mute sets every track; Ctrl-click
  solo (solo safe) is refused by name until the model has it. Fixed: the
  strips read MixState by id order, so after a reorder each showed another
  track's fader. The strips' meters are labelled IN (the state block has
  input peaks only) and the bus's NO DATA; the line above says what is live
  and says the fake engine plays nothing. Post-fader and bus peaks are
  BACKLOG Track 1 item 9.
- **ADR-0012, plugin hosting (M9), accepted in part (#42).** One host per
  insert chain, depth 1, a missed block is silence (64-frame fade-out,
  ramp-in), make-before-break restarts, inserts in the model, PDC by the
  plan builder. The audio thread's futex wake is gated off
  (`kHostWakeEnabled = false`, hosts timer-paced) until a bare-metal
  measurement passes.
- **Native plugins N1 follow-up (#43).** A mono bundle beside each stereo
  one, a side-chain key on the compressor and gate, automation and
  re-activation through the hosts; lv2lint 12/12, VST3 validator 47/47 x 12,
  84 renders through `sonora-plugin-host` bit-exact against N0.
- **`contracts/plugin` accepted as layout 1 (#45, ADR-0012 checklist 1).**
  Block-tagged dense parameters with a last-value table and engine-only
  `values_seq`, `kBypassParam`, the read-only sealed clock region, exit 3 on
  a version mismatch, `plugins/host` and its driver on it.
- **Native reverb, ADR-0011 N2 slice 1 (#47).** An FDN reverb (16 lines,
  three-band decay, freeze) as stereo and mono LV2 and VST3 bundles; T30
  within 2.3 % of the set decay on 171 octave bands, 14 renders through
  `sonora-plugin-host` bit-exact against the reference, lv2lint and the
  VST3 validator pass. Container figures (`brain/research/native-reverb-n2.md`).
- **Native tube drive, ADR-0011 N2 slice 2 (#52), and its gate follow-ups
  (`fix/tube-cpu-gate`).** The SPICE reference harness (ngspice in the
  plugins image, generated netlists checked byte for byte), a 12AX7
  common-cathode stage (Dempwolf law, DK-reduced Newton solve) in
  `common/dsp/triode.h`, and the tube drive as stereo and mono LV2 and
  VST3 bundles with eco (2x), standard (4x) and high (8x) qualities.
  Harmonics within 0.09 dB of ngspice; hot squares to +24 dBFS stay
  finite and in range. Eco meets class B on a quiet reference machine:
  p99.9 at most 45.8 µs over three runs on its three loads (bar 60);
  standard and high are reported over it, as §3.5 allows. The nightly
  gates eco with a runner-calibrated bar (a frozen reference kernel
  scales it), the absolute bar stays the reference machine's
  (`brain/research/native-tube-n2.md`). Transformer, sag and modelled
  noise are later slices.
- **Inserts in the model, protocol 3 (#46)** merged; plan nodes and PDC
  (checklist 3) are in progress on `feature/plan-pdc`.
- **`engine/plugin_client`, the RT half (ADR-0012 checklist 4,
  `feature/plugin-client`).** `PluginClient::process_chain` per InsertChain
  node: blocks numbered per channel from 1, depth-1 collect through
  `collect_output` (NaN/Inf a miss and a fault), fade/ramp state per node
  (M5), swap silence not counted, 8 misses remove the channel (the culprit
  sampled by the control thread), slot generations (H1, moved again on
  retire), the A2.2 copy budget and A2.5 store index, once per node per
  callback, in-place nodes, `kNoStore` (PR #48's sentinel).
  `HostSupervisor`/`SupervisedChain`: a non-RT stub that starts the
  real `plugins/host` binary through `common/platform` (new: writable
  shared regions, regions passed at fixed descriptors, `sealed_against_writes`,
  `lock`), locks the clock and channel regions, checks `--version`, the
  clock seal and the block size at start-up, detaches the clock safely at
  teardown, and restarts make-before-break, at most 3 in 60 s (blocking for
  now; asynchronous before checklist 5). A3: an `nm` ALLOWLIST test holds
  that the audio thread's object references only memcpy, memset, memmove
  and `wake_all`, and `wake_all`'s object only `syscall`; three canaries
  (another platform call, `new`, a `std::string` at -O2) must fail it. The RTSan check of
  `wake_all` is written but skipped: the toolchain is GCC 14 (BACKLOG).
  Called from `Session::process` through `PluginChainRunner` (below); the
  control-server wiring with `host_control.h` (checklist 5) remains.
  Container only.
- **`Session::process` runs real plugin hosts (#59, ADR-0012 checklist
  4).** `PluginChainRunner` in engine/core (ADR-0007's edge runs core ->
  plugin_client) maps the InsertChain node onto `PluginClient`. ctest
  `session_plugins_*` in `tools/build-plugins.sh`: a 2048-frame chain of
  four Debian LV2 limiters (two LSP at 20 ms, two x42-dpl; 2176 with the
  IPC block) lands bit for bit on a dry track at the bus; the in-tree
  look-ahead and eg-amp, a stereo chain on a stereo track (both sides),
  AGain and the LSP limiter as VST3, and an LV2+VST3 chain in one host
  likewise; a host stalled at 20 ms a block is removed after 8 misses while
  the dry track plays on and no `process()` exceeds 1 ms (slowest 28 µs,
  container); no operator new/delete, and outside the sanitizers no
  malloc/free, on the thread inside `process()`. A binding with no slot or
  no node state is laid out unbound, latency 0, so it is never early. Hosts are still started by hand in the test: the engine
  binary starts none until checklist 5.
- **Checklist 5, the RT side (`feature/plugin-chain-control`).** Plugin
  parameter changes reach a chain: `Session::post_plugin_param` queues an
  `RtCommand` in a ring of its own, the callback hands it to the
  InsertChain node naming that store, and `session_plugins_params` moves
  eg-amp's gain to -20 dB and then bypasses it, each heard from the
  callback after the post. `Session::process` reads no clock: the backend
  stamps `AudioBlock::callback_ns` (JACK's cycle time), checked against
  `steady_clock` by the backend conformance suite. A runner with a fixed
  block (the client's) runs only callbacks of that size; a bound chain the
  session does not run plays silence, never dry and early, and the plan
  report shows the latency the plan carries. The control server does not
  post any yet, nor start hosts.
- **The DAW on the owner's Windows laptop (`tools/dev-wsl.sh`, #44).** Debian trixie under WSL2: packages, build, PipeWire with
  pulse-tunnels to WSLg's PulseAudio (Windows speakers and mic), engine +
  Qt UI through WSLg; `selftest` records from the mic (or `--tone`), plays
  back and prints levels. Run end to end 2026-10-13 in a trixie container
  with a real PulseAudio server standing in for WSLg's: mic tone recorded
  at its level, played back, and arriving at the stand-in's speaker sink,
  0 xruns; UI screenshot `docs/images/dev-wsl-ui.png`. **Not yet run under
  WSL itself**: the agent could not run `wsl.exe`, and no Debian distro is
  installed. The owner runs `tools\dev-wsl.ps1`.
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
| VST3 hosting in `plugins/host` (same chain, same protocol) | plugin-engineer | 16 VST3 ctest cases pass in `tools/build-plugins.sh` (AGain, LSP limiter, in-tree crash/mismatch/liar); `brain/research/plugin-host-vst3.md`; yabridge next (BACKLOG Track 4 item 6) |

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
