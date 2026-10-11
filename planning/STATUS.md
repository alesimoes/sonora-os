# Status

The single place to learn where the work stands. Any agent updates this at the
end of its run. Keep it short: what is true now, not a diary.

- **Updated:** 2026-10-11
- **Current phase:** 1 — Bootable appliance
- **Build state:** the engine compiles clean under GCC 14 with `-Wall
  -Wextra` and all three unit tests pass, in the Debian trixie container built by
  `tools/build-engine.sh`. Verified 2026-10-11.
- **ISO builds.** `iso/sonora.iso`, 867 MB, built 2026-10-11. Inspected
  rather than assumed: the engine is present at mode 0755, the service is
  enabled, the `sonora` user is in `audio`, all three tuning files are
  installed, and both bootloaders default to the RT kernel, which is now the
  only kernel in the image.
- **The engine records.** `tools/test-engine-docker.sh` runs it against a real
  PipeWire in a container and verifies the files: 18 tracks -- the UMC1820
  count -- at 48 kHz, quantum 128, zero xruns, zero overruns, -9.0 dBFS which
  is precisely the RMS of the sine fed in, and each track carrying the tone it
  is supposed to carry rather than a neighbour's. Runs in seconds, so it is
  the loop for engine work.
- **The image boots.** `tools/test-boot-qemu.sh`, 2026-10-11: up in about 13
  seconds on `6.12.111+deb13-rt-amd64`, `sonora-engine` running, no failed
  units, 19 of 19 checks passing. Among them: `libjack.so.0` resolves to
  PipeWire's rather than jackd2's; the Pro Audio profile is applied, so ports
  arrive as `...pro-input-0:capture_AUX0`; the engine holds the same PID across
  ten seconds with `NRestarts` at zero; there is no serial login prompt; and
  the operator is not in the `disk` group. The harness also asserts statically
  that both bootloader defaults load the RT kernel, comparing bytes rather
  than names.
- **Still unverified:** no real audio interface has ever been attached, so
  there is still no latency or xrun figure from hardware. Nothing has run on a
  physical machine. `tools/write-usb.sh` has never been executed.
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

## Blocked

| Item | Blocked on |
|---|---|
| Hardware validation | No audio interface has been attached. The target is a Behringer UMC1820; every figure in this repository comes from a container or from QEMU. |

## Next decision needed from the owner

Whether Phase 2 playback targets a fixed 2-track monitor bus first, or the full
mixer topology from the start. The former gets audible playback weeks earlier.
