# Status

The single place to learn where the work stands. Any agent updates this at the
end of its run. Keep it short: what is true now, not a diary.

- **Updated:** 2026-10-12
- **Current phase:** 1 — Bootable appliance
- **Build state:** the engine compiles clean under GCC 14 with `-Wall
  -Wextra` and all six unit tests pass (ringbuffer, wav_writer, takes,
  quit_signals, port_order, keymap), in the Debian trixie container built by
  `tools/build-engine.sh`. Verified 2026-10-12 from a clean build directory,
  and under the CI sanitizer flags (TSan; ASan+UBSan) in the same container
  -- TSan needs `--security-opt seccomp=unconfined` and `setarch -R` there,
  because Docker Desktop's kernel randomises more address bits than TSan
  maps. The writer/disk-thread race TSan found is fixed in `a141e9d`.
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
  units, 19 of 19 checks passing. Among them: `libjack.so.0` resolves to
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
  transport status, footer, track arming and key routing through `ui/keymap`, as a separate process over a
  synthetic engine. `tools/build-ui.sh` builds it and runs 10 tests in the
  `sonora-ui` image (one renders frames under Xvfb); the engine container and
  CI have no Qt and skip it. The digit row is named by its physical key, so
  AZERTY and ABNT2 arm the right track (`keymaps/README.md`). Measured with
  llvmpipe only (`brain/research/qt-shell-render-cost.md`): no GPU figure.
  Open: no compositor or systemd unit for it in the ISO (a compositor ADR is
  architect's); no CI job builds it; whether `app.quit` closes only the UI;
  at M4 `EngineAdapter`/`SyntheticEngine` give way to the control API.
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
| QEMU boot smoke test | distro-engineer | passing, 19 checks |
| `write-usb.sh` against a disk image, five QEMU boots | distro-engineer | passing; physical stick untested |
| Phase 2 session format and timeline model specs, ADR-0009 | architect | in review, `docs/phase2-session-timeline` |

## Blocked

| Item | Blocked on |
|---|---|
| Hardware validation | No audio interface has been attached. The target is a Behringer UMC1820; every figure in this repository comes from a container or from QEMU. |

## Next decision needed from the owner

None open. Decided 2026-10-12: Phase 2 playback starts with a fixed
2-channel monitor bus; the full mixer comes later on the same graph
(`planning/ROADMAP.md`, Phase 2; `brain/specs/session-format.md`).
ADR-0009 (timeline snapshots, residency-gated plan swap) is proposed on
`docs/phase2-session-timeline`, in review.
