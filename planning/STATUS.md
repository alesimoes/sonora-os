# Status

The single place to learn where the work stands. Any agent updates this at the
end of its run. Keep it short: what is true now, not a diary.

- **Updated:** 2026-10-11
- **Current phase:** 1 — Bootable appliance
- **Build state:** engine not yet compiled on Linux. The code is written and
  the CMake project is complete, but nothing in this repository has been run.
  Treat every claim about it as unverified until CI goes green.

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
| GitHub remote | `gh` CLI is not installed; repository not created yet |

## Next decision needed from the owner

Whether Phase 2 playback targets a fixed 2-track monitor bus first, or the full
mixer topology from the start. The former gets audible playback weeks earlier.
