# The LV2 plugin host, measured in a container

What `sonora-plugin-host` (plugins/host) and its engine-role test driver
showed on 2026-10-13. Branch `feature/plugin-host-lv2`. The boundary they
speak is the **draft** contract `contracts/plugin/include/sonora/plugin/protocol.h`,
which awaits rt-engineer review (ADR-0007 §4.3, §6).

## The machine

The owner's Windows host, Docker Desktop, `debian:trixie-slim` with
`tools/Dockerfile.plugins`, run by `tools/build-plugins.sh` with
`--cap-add=SYS_NICE --ulimit rtprio=99 --ulimit memlock=-1`. The driver ran at
`SCHED_FIFO` 80 and the host at `SCHED_FIFO` 70, both with `mlockall`. Same
environment class as `adr-0007-measurements.md`: a shared generic kernel on a
Hyper-V guest. **Every timing below is a container figure, not an appliance
figure.**

## Plugins used [measured]

| Package (trixie main) | Plugin | URI | Reported latency at 48 kHz |
|---|---|---|---|
| `lv2-examples` 1.18.10 | Simple Amplifier (eg-amp) | `http://lv2plug.in/plugins/eg-amp` | none |
| `x42-plugins` 20240611 | x42-dpl Digital Peak Limiter | `http://gareus.org/oss/lv2/dpl#mono` | 64 frames |
| `lsp-plugins-lv2` 1.2.21 | LSP Limiter Mono | `http://lsp-plug.in/plugins/lv2/limiter_mono` | 240 frames (defaults) |

Both limiters require only `urid:map`. With 304 plugins installed, `--list`
marks 7 unusable (a required feature this host lacks; x42 midimap and
zeroconvolv among them).

## Results [measured, `ctest -L plugins`, `tools/build-plugins.sh --measure`]

- **Gain.** eg-amp at 0, −6 and +12 dB, set through the parameter ring while
  running: measured +0.0000, −6.0000, +12.0000 dB, mono and stereo (replicated
  mono instance per channel).
- **Latency is what the plugin says, to the sample.** Noise at −20 dBFS
  through x42-dpl, then through dpl → LSP limiter → eg-amp. Reported 64 and
  304 frames. The delay found by search was 64 and 304. The wet path minus
  the dry path delayed by the reported latency (the PDC plan for two tracks
  on one bus) was **exactly 0** for every sample (both limiters are
  bit-transparent below threshold). One sample either side left 0.199. In
  pipelined mode (depth 1) the host reported 192 = 64 + 128 and the null held
  at 192: the IPC block is absorbed by PDC like any look-ahead.
- **Crash.** `urn:sonora:test#crash` segfaults on a parameter. Host died of
  SIGSEGV; the driver noticed 7 blocks later (non-blocking `waitpid`), read
  `current_instance` = 1 from the still-mapped region and named the plugin.
  All 400 callbacks ran; none overran the period.
- **Deadline.** `urn:sonora:test#sleep` sleeping 20 ms per block: every block
  dropped at the deadline, the host left the plan after 8 consecutive misses
  with the plugin named. No callback overran the period.
- **Validation.** `urn:sonora:test#liar` declares 2 channels and writes 8:
  refused before the chain was offered, by the canary guard behind each
  buffer. A stereo plugin on a mono chain and a missing URI are refused and
  named.

### Round trip per block, sync mode (submit, futex wake, host runs the chain, futex wake back)

| Chain | n | p50 µs | p99 µs | p99.9 µs | max µs | dropped |
|---|---|---|---|---|---|---|
| eg-amp | 20 000 | 61.8 | 251.8 | 418.4 | 7 929 | 6 (0.03 %) |
| dpl → LSP limiter → eg-amp | 20 000 | 75.3 | 324.9 | 429.3 | 615.4 | 0 |
| eg-amp (ctest run) | 2 000 | 67.8 | 258.4 | 328.0 | 358.7 | 0 |

Host-side chain time, worst block: 131 µs (eg-amp), 198 µs (three plugins).
The p50 and p99 agree with the futex round trip in `adr-0007-measurements.md`
(60.6 / 209.2 µs): the IPC dominates, not the plugins.

## Findings

1. **A timed wait overshoots.** The 7 929 µs maximum is one futex wait asked
   for at most 2 ms that returned 5.9 ms late: the driver was not scheduled,
   so the callback overran whatever the host did. The driver now counts a
   block noticed after the deadline as a miss even if the host was on time.
   This is the same tail the earlier measurement saw (3.6 ms). It argues for
   **`pipeline_depth` 1 on the audio thread**: one block of declared latency
   (2.67 ms at 128), and the callback only polls `completed_block` and makes
   one wake syscall. Depth 0 needs a bounded wait on the audio thread, which
   is the auditor's ruling and this tail's exposure.
2. **One input slot and one output slot are enough** if the engine submits
   only when `completed_block == submitted_block`. A late host is then never
   handed new input while it reads the old, and a late output is discarded,
   not played a block late (which would break the declared latency).
3. **Death and lateness need different detectors.** Lateness is seen on the
   audio thread (`completed_block` not advancing). Death needs the control
   thread (`waitpid`/pidfd); the heartbeat covers a host that is alive but
   hung outside a plugin.

## Open, and inputs to the Phase 4 plugin ADR

- Every figure here on the RT kernel, bare metal (BACKLOG, ADR-0007 IPC item).
- Futex versus eventfd stays a seam choice (`plugins/host/src/ipc.h`); the
  engine's half moves to `common/platform` when `engine/plugin_client` lands.
- One host per chain (this branch) versus per track or per plugin: crash
  blast radius against process count, not measured.
- Runtime canary check covers 64 words past `max_frames`; a write inside
  `[frames, max_frames)` at runtime is only caught at validation.
- Plugin editors: not hosted. Under ADR-0010 they are parentless X11
  toplevels placed by labwc via Xwayland. The opaque parent-window token
  belongs in `HostControl`'s editor operation (not drafted yet) and is empty
  on the appliance. Untested with a real editor (BACKLOG).
- State save and restore, LV2 worker, presets: not implemented.
