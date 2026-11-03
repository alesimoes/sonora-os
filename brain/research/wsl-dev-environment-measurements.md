# WSL dev environment: first end-to-end run (2026-11-02/03)

Measured on the owner's laptop: Windows 11 Enterprise 10.0.26300, WSL2,
distro `Debian` 13.5 (trixie) with `systemd=true`, WSLg 1.0.66 (built
2024-09-12, PulseAudio 16.1 inside), 16 logical CPUs, Docker Desktop running
beside it. No GPU path: the UI drew on llvmpipe (software). Scripts as of
`8a22ddaf` (#284), plus the fixes of the PR that adds this note.

This is the development path, not the appliance (ADR-0007 M7, ADR-0008):
`--no-memlock`, no RT scheduling, sound through pulse-tunnel -> WSLg ->
RDP. Nothing here says anything about xruns on the appliance.

## Timings

| Step | Result | Time |
|---|---|---|
| `status` | correct, exit 1 when the DAW is down | 2.5 s |
| default flow, root, build from 2026-11-01 present | deps no-op, incremental build, engine + UI up | about 7 min to the first frame |
| `user` (1st) | created `sonora` (uid 1000), wsl.conf `[user] default=sonora`, terminated Debian only | 3.7 s |
| `user` (2nd) | "already the default user; nothing to do" | 7.7 s |
| `deps` | all 30 packages present | 3 s |
| `build`, empty build dir (new user) | engine, UI, ctl, tui, Release, -j16 | 563 s |
| `selftest --tone` | passed, before the socket fix | 27 s |
| `selftest --tone` on a just-started distro, after the fix | passed | 23 s |
| `selftest` (microphone) | passed, but the mic is silent (see below) | 12 s |
| UI first frame under WSLg | llvmpipe | 1.7 to 3.3 s |

Docker Desktop's containers kept running through every `wsl --terminate
Debian` (one had been up for 51 min, checked with `docker ps` before and
after); `docker-desktop` stayed Running.

## Audio numbers

- Graph clock after every start: `clock.rate=48000 clock.quantum=128`,
  force-rate and force-quantum 0. WSLg's own server runs at 44100 Hz and the
  tunnel resamples.
- Engine counters (`sonora-ctl state`), every run and selftest:
  `xruns=0 overruns=0`. Over a run of about 4 min with the UI open and three
  takes: still 0.
- PipeWire's view of the same runs (`pw-top`, ERR column, which counts
  per-node xruns of the graph): `sonora` 87 to 269, `wslg-mic` 504 to 1588,
  `wslg-speakers` 13 to 93, growing with time. The engine's counter and the
  graph's disagree; the graph's count is driven by the Dummy driver the
  tunnels sit on. Not investigated further.
- Tone through the engine to Windows (no human listening): a 440/660 Hz tone
  at -12.2 dBFS was recorded by the engine (take peak -12.2 dBFS, RMS
  -15.3 dBFS), played back, and `parecord -d RDPSink.monitor` on WSLg's own
  server saw -9.6 dBFS for the 5 s of the playback and -inf elsewhere. So the
  path engine -> PipeWire -> pulse-tunnel -> WSLg RDPSink is shown to carry
  the signal. What Windows does with RDPSink (mixer, speakers) was not
  checked: that needs a human listening.
- Microphone: WSLg's RDPSource gave -78 to -80 dBFS, or exact zero, in every
  test, also when read directly with `parecord` and no PipeWire. Either the
  laptop has no active mic or Windows privacy denies WSLg; not resolved.
- Round-trip latency: not measured. There is no loopback tool for this path,
  and an acoustic loopback through the laptop's speaker and mic would need
  the mic working and someone in the room.

## Failures found

1. **systemd's pipewire.socket (fixed).** With `systemd=true` and a non-root
   user, systemd listens on `/run/user/1000/pipewire-0` but cannot start
   `pipewire.service`: "Failed to queue service startup job: Unit
   dbus.service not found" (no `dbus-user-session`). The first PipeWire
   client fails the socket unit, the script waited 10 s for the speakers and
   restarted PipeWire. `release_systemd_socket` now stops the socket first;
   the next start went straight to our PipeWire.
2. **WSLg's PulseAudio server stops answering (not fixed, worked around by
   the operator).** Seen three times: first `pactl info` times out, a few
   minutes later the socket refuses connections. Each time WSLg's own log
   (`/mnt/wslg/pulseaudio.log`) fills with `[rdp-source] asyncq.c: q
   overrun, queuing locally`, starting while the engine had the mic tunnel
   linked. Once (root run, PipeWire up for 13 h) the sink tunnel's buffer
   then filled to 38 MB against a 4 MB max without a single "connection
   failure" line, so the old `tunnel_dead` check could not see it.
   Reading RDPSource with `parecord` alone for 5 s did not trigger it.
   Keeping the mic tunnel uncorked (`node.always-process`) stopped it after a
   selftest but not during a run with the UI open, so that change was
   dropped. `wsl --terminate Debian` restarts WSLg for this distro and
   cures it; `check_wslg` already tells the operator so. A full sink buffer
   now also counts as a dead tunnel, so a later start restarts PipeWire.
   A newer WSLg (`wsl --update`) was not tried: nothing was installed on
   the host.
3. **Default flow exited 143 (fixed).** `dev-wsl.ps1` with no arguments,
   ended by `stop`, returned 143 while `run` returned 0. Both now return 0.
   Verified by reading only at #292: the fix mapped any 143 to 0. Since
   then `stop` leaves a marker and `dev-wsl.sh run` maps 143 to 0 only when
   it is there; `tools/test-dev-wsl.sh` checks that mapping on Linux. That
   `dev-wsl.ps1` passes the 0 through is still verified by reading only,
   not on the laptop.

## What a human must still check

Hearing the tone from the laptop speakers; the mic (Windows Settings >
Privacy > Microphone for WSLg); that the UI window is usable at llvmpipe
speed.
