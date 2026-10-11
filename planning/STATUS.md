# Status

The single place to learn where the work stands. Any agent updates this at the
end of its run. Keep it short: what is true now, not a diary.

- **Updated:** 2026-10-11
- **Current phase:** 1 — Bootable appliance
- **Build state:** the engine compiles clean under GCC 14 with `-Wall
  -Wextra` and both unit tests pass, in the Debian trixie container built by
  `tools/build-engine.sh`. Verified 2026-10-11.
- **ISO builds.** `iso/sonora.iso`, 867 MB, built 2026-10-11. Inspected
  rather than assumed: the engine is present at mode 0755, the service is
  enabled, the `sonora` user is in `audio`, all three tuning files are
  installed, and both bootloaders default to the RT kernel, which is now the
  only kernel in the image.
- **Still unverified:** nothing has ever booted. No audio has been recorded.
  The engine has never run against an audio server, real or virtual, so every
  claim about its behaviour is still a claim about code rather than an
  observation. The next task is the QEMU smoke test.
- A review of the initial commit already found three missing includes and a
  case where losing the audio server left the engine showing REC while
  capturing nothing (fixed in `dafebe6`). That is the expected yield from
  uncompiled code, and the reason CI is the next task rather than features.

## In flight

| Item | Owner | State |
|---|---|---|
| First compile and `ctest` run | distro-engineer | done, both tests pass |
| `live-build` configuration | distro-engineer | builds; contents verified |
| Autostart into the engine | distro-engineer | unit enabled in the image, never booted |
| QEMU boot smoke test | distro-engineer | next up |

## Blocked

| Item | Blocked on |
|---|---|
| First push | `gh` CLI is not installed. The remote is configured as
  `github.com/alesimoes/sonora-os`; the repository itself still has to be
  created before `git push` will work. |

## Next decision needed from the owner

Whether Phase 2 playback targets a fixed 2-track monitor bus first, or the full
mixer topology from the start. The former gets audible playback weeks earlier.
