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
  mono instance per channel), at depth 1 and depth 0. A change pushed in
  callback p lands in exactly the first block submitted at or after p (checked
  per block, eg-amp does not smooth).
- **Latency is what the plugin says, to the sample.** Noise at −20 dBFS
  through x42-dpl, then through dpl → LSP limiter → eg-amp. Reported 64 and
  304 frames. The delay found by search was 64 and 304. The wet path minus
  the dry path delayed by the reported latency (the PDC plan for two tracks
  on one bus) was **exactly 0** for every sample (both limiters are
  bit-transparent below threshold). One sample either side left 0.199. In
  pipelined mode (depth 1) the host reported 192 = 64 + 128 and the null held
  at 192: the IPC block is absorbed by PDC like any look-ahead.
- **Crash.** `urn:sonora:test#crash` segfaults on a parameter. Host died of
  SIGSEGV; the driver noticed 7 to 8 blocks later (non-blocking `waitpid`),
  read `current_instance` = 1 from the still-mapped region and named the
  plugin. All 400 callbacks ran; none overran the period. Same at depth 1
  and depth 0.
- **Deadline.** `urn:sonora:test#sleep` sleeping 20 ms per block: every block
  dropped, the host left the plan after 8 consecutive misses with the plugin
  named (block 108 at depth 1, 107 at depth 0). No callback overran the
  period.
- **NaN and Inf at run time.** `urn:sonora:test#nan` writes NaN/Inf on every
  fourth sample for 100 blocks: none reached the driver, the host counted
  3 200 replaced samples against that plugin and 0 against eg-amp after it,
  and the chain was bit-identical to its input once the plugin stopped.
- **Validation.** `urn:sonora:test#liar` declares 2 channels and writes 8:
  refused before the chain was offered, by the canary guard behind each
  buffer. A stereo plugin on a mono chain and a missing URI are refused and
  named.

### Round trip per block (submit, futex wake, host runs the chain, futex wake back)

Measured by the `rtt` scenario, which sleeps on `done_word` to time the IPC.
That wait is a measurement harness, not a protocol mode: the engine's audio
thread never waits on a host (finding 1).

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
   one wake syscall. **Decided in review (2026-10-13): depth 1 is the
   default and the protocol has no wait on the audio thread.** Depth 0
   survives only as poll-only (submit early, look once late in the same
   callback), pending the auditor's ruling on the single `FUTEX_WAKE`.
2. **One input slot and one output slot are enough** if the engine submits
   only when `completed_block == submitted_block`. A late host is then never
   handed new input while it reads the old, and a late output is discarded,
   not played a block late (which would break the declared latency).
3. **Death and lateness need different detectors.** Lateness is seen on the
   audio thread (`completed_block` not advancing). Death needs the control
   thread (a pidfd in the engine; the driver polls `waitpid`); the heartbeat covers a host that is alive but
   hung outside a plugin.
4. **Bundle paths need `lilv_new_file_uri` on an absolute path.** A
   hand-built `file://` URI with a relative path finds nothing (regression:
   `plugin_host_probe_liar`).
5. **Zero over-period callbacks cannot be asserted in the container.** One
   VM stall of about 5.9 ms comes every ~20 000 blocks, so the driver allows
   `kOsNoiseCallbacks = 2`; the bare-metal RT run is where zero is expected.
6. **Name the culprit from the streak, not the last read.** At depth 1 the
   eighth miss came 21.3 ms after the slow block was submitted, just after
   the 20 ms sleep ended, and `current_instance` read -1 at removal. The
   engine must remember the instance it saw running during the misses.
7. **Everything the host writes is untrusted** (engine owner's review of
   PR #26). The first draft computed plane addresses from offsets in the
   shared header, which a scribbling plugin could rewrite to aim the engine's
   RT `memcpy` anywhere. Each side now keeps a private `Layout`; host-written
   words go through `checked_*` before use (contract header, "Trust").
8. **The URID map is on the RT path.** Plugins call `urid:map` from `run()`.
   The first version took a mutex and grew a `vector<string>` whose growth
   moved short (SSO) strings under earlier `unmap()` pointers. It is now an
   open-addressed table filled once per slot by CAS, with text in a fixed
   arena: no lock, no allocation after construction (`test_urid_map`).
9. **`PR_SET_PDEATHSIG` follows the forking thread.** It fires when the
   thread that forked the host exits, and never if the engine died before
   the `prctl`. The host checks `getppid()` against `--engine-pid` after the
   `prctl` and in its idle loop; the engine must fork hosts from its
   long-lived control thread and watch them with a pidfd.

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
