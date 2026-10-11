# Status

The single place to learn where the work stands. Any agent updates this at the
end of its run. Keep it short: what is true now, not a diary.

- **Updated:** 2026-10-11
- **Current phase:** 1 — Bootable appliance
- **Build state:** engine not yet compiled on Linux. The code is written and
  the CMake project is complete, but nothing in this repository has been run.
  Treat every claim about it as unverified until CI goes green.
- A review of the initial commit already found three missing includes and a
  case where losing the audio server left the engine showing REC while
  capturing nothing (fixed in `dafebe6`). That is the expected yield from
  uncompiled code, and the reason CI is the next task rather than features.

## In flight

| Item | Owner | State |
|---|---|---|
| First compile and `ctest` run | distro-engineer | not started |
| `live-build` configuration | distro-engineer | skeleton committed, untested |
| Autostart into the engine | distro-engineer | unit written, untested |

## Blocked

| Item | Blocked on |
|---|---|
| ISO build | Docker daemon is not running on the owner machine |
| First push | `gh` CLI is not installed. The remote is configured as
  `github.com/alesimoes/sonora-os`; the repository itself still has to be
  created before `git push` will work. |

## Next decision needed from the owner

Whether Phase 2 playback targets a fixed 2-track monitor bus first, or the full
mixer topology from the start. The former gets audible playback weeks earlier.
