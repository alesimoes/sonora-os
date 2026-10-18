# ADR-0012 — Plugin hosting in the engine (M9): wake, granularity, misses, inserts and PDC

- Status: **Accepted in part**, 2026-10-13.
  - **Accepted (architect)**, as engineering choices inside ADR-0007 §4.3 and
    ADR-0011 §1.5–1.6: §2 (one host per insert chain), §3 (a missed block
    is silence), §4 (`HostControl`), §5 (the five contract items), §6
    (inserts in the model and the session format), §7 (plan nodes and PDC),
    §8 (the control API).
  - **Ruled by the rt-safety-auditor** (PR #42, 2026-10-13), conditions
    folded in: A1 (the one broadcast wake, §1.1) **accepted with
    conditions, provisional**: the wake gate stays **off** and hosts run
    timer-paced (§1.3) until the bare-metal measurement in Unverified 1
    passes. A2 (§5.1) and A3 (Consequences) accepted with conditions.
    Findings H1, M1–M3 and L1–L4 are applied in the sections they name
    ("Rulings of the rt-safety-auditor" maps them).
  - **Two owner decisions**, each with a default this ADR builds on until
    the owner rules ("For the owner"). Neither blocks the contract.
- Date: 2026-10-13
- Decided by: architect. This is the M9 plugin ADR that ADR-0007 §4.3, §9
  (M9) and §10 (owner amendment) require before `engine/plugin_client`, and
  that ADR-0011 §1.4, §1.6 and §2.6 defer to.
- Numbering: follows ADR-0011 (native plugin suite).
- Evidence: `brain/research/plugin-host-lv2-container.md` (the host
  measured, PR #26), `brain/research/adr-0007-measurements.md` §3–4 (IPC
  costs), `brain/research/plugin-host-vst3.md` (VST3 threading). Every
  timing in them is a **container figure on a Hyper-V guest with a generic
  kernel**, not an appliance figure. This ADR says where a decision rests
  on one.
- Plan shape: PR #41 (`feature/playback-m4`, open), whose
  `engine/core/include/sonora/playback_plan.h` already carries
  `PlanEdge::delay_frames` and `delay_line`, and whose `Session::process()`
  already runs per-edge stereo delay lines sized by `max_delay_frames`.
  §7 builds on that shape. If #41 changes before it merges, §7 follows it.

## Context

Three gates block `engine/plugin_client`:

1. **The contract is a draft.** `contracts/plugin/include/sonora/plugin/protocol.h`
   is `kLayoutVersion` 0 with `TODO(accept)` items: block-tagged
   parameters with a last-value table, format of parameter values,
   crash/restart and region reuse, plan-swap behaviour, versioning; and
   (BACKLOG Track 4 item 2) the auditor's ruling on one `FUTEX_WAKE` per
   callback.
2. **No M9 ADR.** ADR-0007 §4.3 left open how the audio thread wakes the
   host, how many plugins a host process carries, and who owns the worker
   threads. ADR-0011 §1.6 left open what a missed block plays. `HostControl`
   was a one-line sketch.
3. **Inserts have no home.** `common/model` and session format v1 have no
   inserts (`session-format.md`, "Not in version 1"). The playback plan has
   no node that runs a plugin chain, and its `delay_frames` are always 0.

What the measurements already settle (container, `[measured]`):

- Futex and eventfd cost the same: round trip p50 about 60 µs, p99
  210–245 µs, against a 2 667 µs period at quantum 128.
- Spinning at `SCHED_FIFO` hits RT throttling (stalls of 46–51 ms). Ruled
  out by ADR-0007 (update 1, item 2).
- A timed wait asked for at most 2 ms returned 5.9 ms late. **Decided in
  the PR #26 review: depth 1 is the default and the audio thread never
  waits on a host.**
- Reported latency equals measured delay to the sample, and in depth 1 the
  IPC block (128 frames) added to it nulled to exactly 0 against the dry
  path delayed by the same figure. PDC as one mechanism works.
- Crash, deadline overrun, NaN/Inf and a lying plugin were all contained,
  with no callback over its period. The culprit must be named from the
  miss streak, not the last read (finding 6).
- Hosts must be forked from a long-lived thread and watched with a pidfd
  (finding 9).

What the hard constraints say:

- The real-time path cannot allocate, lock or block. A wake syscall never
  waits on another thread's progress, but on the RT kernel it can sleep on
  a contended futex hash-bucket lock (§1.1); whether one is allowed in the
  callback was the auditor's to rule (ADR-0007 §4.3), and the ruling makes
  it wait for a bare-metal measurement.
- Losing a take is the worst outcome. Capture is pre-insert
  (`plugin-latency-compensation.md`), so **no decision here can touch what
  lands on disk.** That is checked again in §7.
- The image boots the same way on hardware we have never seen. Plugin
  hosting must fail loudly at start-up when the host binary does not match
  the engine (§5.5), and must never stop the engine from recording.

## Decision

### 1. How the audio thread wakes the hosts

**1.1 One shared futex wake per callback, broadcast (accepted, provisional,
gate off until measured).**

- The engine creates one small **clock region** per session (a sealed memfd
  in `common/platform`, like the state block). It holds:
  - `tick`, a `uint32_t` word the audio thread increments once per callback
    in which it submitted to at least one host;
  - `callback_ns`, the `CLOCK_MONOTONIC` time at which the **server's
    period** started, and `period_ns`. These pace the timer path (§1.3) and
    the host's own diagnostics. Since #68 (#68 code review, item 2) it is
    the backend's stamp, `contracts/backend` `AudioBlock::callback_ns`: for
    JACK, `jack_get_cycle_times`' current_usecs, the start of the server's
    cycle, not the moment Sonora's callback began, which is later by the
    server's own scheduling and any client ahead of Sonora in the graph.
    Hosts pacing on it therefore wake relative to the period, and the
    offset of §1.3 absorbs that lead. A period the backend could not stamp
    carries the last stamp forward by the period. The callback reads no
    clock: `CLOCK_MONOTONIC` is a vDSO read **only when the clocksource
    supports it** (`tsc`), and with `hpet` or `acpi_pm` it would be a real
    syscall in the callback. The backend conformance check
    `callback_ns_is_the_monotonic_clock` holds that the stamp is on the
    engine's `CLOCK_MONOTONIC` (fake, jackd2, PipeWire). Start-up still
    reads `/sys/devices/system/clocksource/clocksource0/current_clocksource`
    and reports a non-`tsc` source in the health flags (Unverified 9), for
    the rest of the engine.
- **Read-only is enforced by the descriptor, not left to the host.** The
  clock region crosses `SCM_RIGHTS` as the `O_RDONLY` `/proc/self/fd`
  reopen, sealed with `F_SEAL_FUTURE_WRITE` where the kernel has it,
  exactly the path `common/platform/src/posix/ipc.cpp:179-186` already uses
  for the state block; a `PROT_WRITE` mapping of it fails with `EACCES`.
  The contract states this. So a scribbling plugin in one host cannot forge
  a wake or a `callback_ns` for the others.
- Every host sleeps on `tick` with a shared `FUTEX_WAIT` and a timeout
  (100 ms, which also paces its heartbeat), in this order, written into
  `protocol.h`'s rule text: **load `tick` (acquire) first, then check
  `submitted_block`, then `FUTEX_WAIT(tick, seen)`**, as
  `plugins/host/src/main.cpp:390-396` does today for `request_word`. A
  submit between the check and the wait has already changed `tick`, so the
  wait returns at once. Reversed, a lost wake costs the 100 ms timeout,
  about 37 misses, and the chain is removed at 8.
- When the gate is on, the audio thread, **after the last `InsertChain`
  node of the plan has submitted** (§7.2), does `tick.fetch_add(1,
  release)` and **one** `FUTEX_WAKE` with `INT_MAX` waiters on it. Not one
  wake per host: one syscall per callback whatever the host count. It is
  skipped when nothing was submitted, never retried, and its result and
  errno are ignored. It never calls `FUTEX_WAIT` and never spins. `tick` is
  incremented whether the gate is on or off, so turning the gate on changes
  only which call the host sleeps in (§1.3), not what it reads.
- **What the wake costs, from the kernel's design (auditor, not
  measured):** `FUTEX_WAKE` takes the futex hash-bucket lock, walks the
  waiters, and wakes each one after releasing it: O(N waiters) work in the
  callback, possibly with IPIs. On `PREEMPT_RT` (the appliance runs
  `rt-amd64`) that bucket lock is an rt_mutex-backed `spinlock_t`: if it is
  contended, **the audio thread sleeps on it**, with priority inheritance,
  for the holder's critical section. Shared futexes hash into the global
  table, so the holder can be any process on the box, and every host
  waiting on the one `tick` lands in the same bucket. The wake is therefore
  **not** non-blocking on the RT kernel; its cost is PI-bounded and
  plausibly small, but unproven. That is why the gate is off by default.
- A host woken with nothing new for it (it was not submitted to, because
  it was still busy) re-reads its own `submitted_block` and sleeps again.
- `request_word` leaves `ChannelHeader`; `done_word` stays, for the control
  thread waiting for `Ready` and for measurement harnesses only.

**1.2 Futex, not eventfd.** An eventfd cannot broadcast cleanly: in
counter mode the first reader consumes the wake, and leaving it unread
makes every poller spin on a level-triggered fd. So eventfd means one write
per host per callback, *N* syscalls. Futex gives one. The seam keeps the
mechanism replaceable: `common/platform/wake.h`, `WakeWord` and
`wake_all(WakeWord&) noexcept SONORA_RT` on the engine side,
`wait_for_change(const WakeWord&, uint32_t seen, timeout)` on the host side.
On Windows (ADR-0008) the same seam is a named event.

**1.3 Timer-paced hosts: the default path until the wake is measured.**
This is what ships first, and it needs no change to the contract. Each
host sleeps with an absolute `clock_nanosleep` until `callback_ns +
period_ns + offset` (offset 150 µs, placeholder) and then checks
`submitted_block` (after loading `tick`, as in §1.1); if the callback was
late, it sleeps 50 µs and checks again, at most until half a period has
gone. The cost, stated: hosts wake on a guess, every host takes a timer
wake every period even when idle, and the time left to process shrinks by
the offset plus the timer's jitter (rtload woke 53 µs p50, 252 µs p99 late
in the container). Expect more dropped blocks than with the wake. It
becomes the permanent decision if the measurement in Unverified 1 fails.

**1.4 The gate.** The clock region, the per-host header and every ordering
rule are the same under §1.1 and §1.3. Only whether `wake_all()` is called,
and whether hosts sleep in `FUTEX_WAIT` or `clock_nanosleep`, differs. So
the contract (§5) is accepted now, and `engine/plugin_client` calls
`wake_all()` behind one `constexpr bool kHostWakeEnabled`, **default
`false`**. The host learns which mode to use from the clock header (a
`wake_mode` field written by the control thread at creation), so one host
binary serves both. The gate turns on only by an amendment recording that
Unverified 1 passed: with 18 and 64 waiting hosts, under `stress-ng
--futex` from unrelated processes for at least 1 h, the wake's max inside
the callback stays under 5 % of the period at quantum 128 (about 130 µs),
and its p99.9 and max are reported for the case where every host is in a
wait at once.

**1.5 Depth 0 leaves the v1 contract.** `pipeline_depth` stays in the
header, but `header_valid` accepts only 1. Depth 0 (submit early, look once
late in the same callback) saves one block of latency, which matters only
for live monitoring through plugins, and it drops a block whenever wake
plus chain exceeds what is left of the callback. The only figures we have
include a 5.9 ms overshoot. Re-admitting depth 0 needs the bare-metal
figures (Unverified 1) and an amendment. Low-latency monitoring is the
Pro Tools answer to this instead: an armed track can monitor its input
without its inserts (a DAW 4/5 mixer feature, not a protocol mode).

### 2. Process granularity: one host per insert chain

- **A host process runs exactly one insert chain**: the inserts of one
  track, one aux or bus, or the master. Up to `kMaxInsertsPerTrack` = 10
  instances (Pro Tools' A–J), inside the contract's `kMaxInstances` = 16.
- A chain with no active insert has no host and no `InsertChain` node.
- **Parallelism comes from the kernel scheduler.** Each host has one
  processing thread at `SCHED_FIFO` 70, below the audio thread (80). Hosts
  of independent chains run on whatever cores are free. The engine runs no
  worker pool for plugins. This answers the PDC spec's open question 1 for
  plugins: the workers are the hosts' threads, and the engine owns none.
  The engine's own graph (sources, faders, sums, delay lines) stays serial
  on the audio thread; it is cheap (block work measured at 0.5–1 µs p50).
- **Serial stages cost a block each.** With depth 1, a chain's output for
  block N is played in callback N+1. A bus chain fed by track chains
  therefore sees its input one block later, and the master chain one more.
  Track inserts → bus inserts → master inserts is three blocks, 8 ms at
  quantum 128. Each is declared latency, and PDC absorbs it (§7.3). This is
  the price of never waiting; it applies to playback and to monitoring
  through inserts, never to capture.
- Crash blast radius is one chain. Process count is the number of chains
  with inserts. Locked memory per host is the open cost (Unverified 3).

### 3. What a missed block plays: silence, ramped back in

- A block not collected (host still busy, late, dead or removed) makes the
  `InsertChain` node output **silence** for that block, and the miss is
  counted on the chain and on the instance seen running.
- On the first block collected after a miss, the node **ramps that block
  in linearly** from 0, so recovery is not a step.
- **The onset is faded out, not cut (auditor L4).** The audio that should
  follow was never returned, so it cannot be faded; what clicks is the step
  from the last sample played to zero. On the first missed block the node
  outputs, per channel, a linear decay from the last sample it played to 0
  over 64 frames (1.3 ms at 48 kHz), then silence. It is a ramp of one
  value, not repeated music, and costs one stored float per channel and no
  latency. Holding back a short tail of every block so that real audio can
  be faded was rejected: it adds that tail to every chain's latency, all
  the time, for a fault event. Unverified 6 judges both edges by ear.
- With one input slot (protocol rule 1), a host late for block N−1 also
  costs block N, which is not submitted. One late host is therefore at
  least two silent blocks on that path.
- **8 consecutive misses** remove the channel from use (the audio thread
  stops submitting, outputs silence, and raises a flag); the control
  thread then kills the host and applies the restart policy (§5.3). The
  operator is told which plugin, named from the streak (finding 6).
- **During a bounce or freeze, any miss fails the render** (extends
  ADR-0011 §1.6 from master inserts to every chain). A file with a hole is
  never produced.
- **What is never played:** the undelayed dry signal (it jumps in time),
  and the dry signal delayed by the chain's latency (below). The previous
  block repeated is not played either.

### 4. `HostControl`: engine ↔ host, and who talks to whom

- **The UI never talks to a host.** It is a client of the engine only
  (ADR-0007 §3, ADR-0011 §2.6). Every plugin request goes UI → engine
  control socket → engine control thread → host.
- **Engine ↔ host:** one `AF_UNIX` `SOCK_SEQPACKET` socketpair per host,
  created by the engine's control thread when it forks the host, inherited
  as fd 3. ~~The region memfd and the clock memfd cross on it with
  `SCM_RIGHTS`.~~ (Amended 2026-10-13, below: they stay inherited
  descriptors.) Messages are framed and strictly decoded by the same rules
  as `contracts/api/wire.h` (type byte, little-endian fields, capped
  strings, refuse truncation and trailing bytes). The message set lives in
  `contracts/plugin/include/sonora/plugin/host_control.h`:

| Direction | Message | Purpose |
|---|---|---|
| engine → host | `Load` | chain: per instance format, identity (LV2 URI or VST3 class id), state bytes, dense parameter count |
| host → engine | `Hello` | layout version, wire version, pid; the host's first message, sent after the engine's `EngineHello` (amended 2026-10-13, below) |
| host → engine | `Loaded` / `Refused` | mirrors `HostState`, with the `Refusal` and its text |
| engine → host | `GetState` (instance) → `State` (bytes) | save, and make-before-break (§5.3) |
| engine → host | `DescribeValue` (instance, param, value) → `ValueText` | display text; for VST3 a controller call, so it runs on the host's main thread |
| engine → host | `OpenEditor` (instance, parent token) / `CloseEditor` | the opaque parent-window token from ADR-0007 update 1 item 6; empty on the appliance (ADR-0010 floats parentless X11 toplevels) |
| host → engine | `EditorClosed`, `ParamTouched` (instance, param, value) | a value changed in the plugin's own editor, at human rate |
| engine → host | `Shutdown` | clean exit; `shutdown` in the header stays for the RT-side path |

- Socket traffic is control-thread only, at human or 100 Hz rates, never
  per block. Every byte from the host is untrusted, as in the header.
- **Scanning** is not done by serving hosts: one `sonora-plugin-host
  --probe` process per bundle (BACKLOG), run by a scan worker the control
  server owns, results cached in the persistence partition keyed by bundle
  path, size and mtime. A probe that crashes or times out marks the bundle
  blocked, with the reason. The catalog reaches clients as in §8.
- **Hosts are forked from the control thread** (finding 9), with
  `--engine-pid`, and watched by a pidfd in the control thread's poll set.
  The heartbeat is checked every 50 ms; one older than 500 ms while `Ready`
  is a hang outside a plugin, and the host is killed (`SIGKILL`), which
  then follows the crash path.

**Amendment, 2026-10-13: HostControl transport and handshake** (PR #51,
`host_control.h` wire version 1). Proposed by plugin-engineer. The
rt-engineer co-review on PR #51 found it acceptable on the merits. It
binds once the PR merges.

1. **The regions stay inherited descriptors; no `SCM_RIGHTS`.** The
   channel region and the clock's read-only reopen are placed by the
   spawner at fixed descriptors and named on the command line
   (`--shm-fd`, `--clock-fd`), as layout 1 already does. The socket
   carries messages only. Reasons:
   - The guarantee of §1.1 and auditor M1 does not depend on how the
     descriptor travels. The clock is still the `O_RDONLY` reopen of a
     memfd sealed with `F_SEAL_FUTURE_WRITE`, so a `PROT_WRITE` mapping
     fails either way.
   - Leakage is no worse. Both ends of the pair, and both regions, are
     close-on-exec from birth (`ControlChannel::pair`). Only the child
     gets them, through `dup2` between fork and exec. No other host forked
     meanwhile can inherit them.
   - The host carries no descriptor-receiving code at all. A host that
     receives no descriptors cannot be handed the wrong one, and no
     message brings a resource a receiver must remember to close (the
     reason `contracts/api` frames large payloads instead of passing
     memfds). Under wire 1, a descriptor arriving on the socket is a
     protocol fault (`ControlChannel::receive` refuses it when no region
     is asked for).
   - It is one mechanism instead of two. The regions exist before the
     fork in either design, so passing them after it would add a round
     trip and a failure mode, and gain nothing.
   - What it costs: the spawner must place three descriptors (3 socket,
     then the regions), and a later protocol that wants a region created
     after start-up (the telemetry memfd of §8) needs `SCM_RIGHTS` then.
     That is a wire-version bump when it comes.
2. **The engine speaks first.** The first datagram on the pair is the
   engine's `EngineHello` (wire, layout and clock versions). The host's
   `Hello` is the first thing *the host* sends, after it has read
   `EngineHello` and checked the regions. Reason: §5.5 requires a host
   that does not speak the engine's versions to exit 3 "before sending
   anything on the socket". It can only do that if it learns the engine's
   wire version before it sends. The first bytes of both hellos (frame
   header, type byte, u32 wire version) never move, so either side can
   name a mismatch whatever else changed.

### 5. The contract items, resolved

`contracts/plugin` is owned jointly by rt-engineer and plugin-engineer
(ADR-0007 §4.3, §6). These are the decisions they implement; field names
are theirs to settle in the acceptance PR.

**5.1 Parameters: block-tagged, with a last-value table.**

- `ParamChange` becomes `{uint64_t block; uint32_t instance; uint32_t
  param; uint32_t offset; float value;}`, padded to 24 bytes.
  - `param` is a **dense index** into the instance's parameter list as the
    scan described it, never a VST3 ParamID or an LV2 port index on the
    wire; the host maps it with the descriptor it built itself.
  - `block` is the `submitted_block` the change belongs to, `offset` the
    frame inside it (`< submitted_frames`).
- **The audio thread is the ring's only producer.** UI changes, editor
  touches and (DAW 4) automation reach it as `RtCommand`s (ADR-0007 §2's
  second form) and it tags them with the block it is about to submit. The
  ring stays SPSC.
- **The host consumes in order and never drops**: entries for the block it
  is processing apply at their offset; entries for an older block apply at
  offset 0 and are counted as late; it stops at the first entry for a
  future block and leaves it in the ring.
- **Delivery:** VST3 at the offset (`IParameterChanges`, sample offset);
  LV2 at block start, because LV2 control ports are per `run()`. Splitting
  `run()` at offsets for LV2 is allowed by the contract and not done in v1
  (ADR-0011 Unverified 6 decides whether it is ever needed).
- **Bypass** of an instance is a reserved `param` value
  (`kBypassParam = 0xFFFFFFFF`), so it is block-tagged and automatable like
  any parameter, with no plan swap. The host implements it
  latency-compensated: the plugin's designated bypass parameter if it has
  one (VST3 `kIsBypass`, LV2 `lv2:enabled`), otherwise a host-side delay of
  that instance's reported latency. Toggling bypass never shifts timing
  (ADR-0011 §1.5).
- **Last-value table.** The region gains `float last_value[param_slots]`
  (atomics), `param_slots` = the chain's dense parameter total, capped at
  `kMaxParamsPerChain` = 8192. Parameters past the cap are not automatable
  from Sonora; the catalog says so, and the plugin still saves them in its
  state.
  - The engine keeps the **authoritative** copy in private memory. The
    control thread maps the chain's stable id (§7.1) to a store index, so
    the store survives a host replacement; the audio thread only ever sees
    the index. The shared table is a mirror for the host and is never read
    back.
  - The audio thread writes the table entry (relaxed atomic store) before
    pushing the ring entry. A push refused because the ring is full bumps
    **`values_seq`**, a `uint32_t` counter in the region that **only the
    engine writes** (release). The host keeps the last `values_seq` it saw
    in its own memory; when the region's differs, it loads it (acquire),
    re-reads the whole table and applies every value that differs. No
    host-written word sits on this path, so a set that lands while the host
    reads the table bumps the counter again and is picked up next block
    (auditor M3; a dirty flag cleared by the host would lose it). A full
    ring therefore delays a value by at most one block and never loses it.
  - **Precedence inside one block (auditor L1).** At the start of block N
    the host first drains the ring entries for blocks ≤ N in order, then,
    if `values_seq` moved, applies the table, which is never older than
    any ring entry already pushed for those blocks (the table entry is
    written before its ring entry). Ring entries for block N at a non-zero
    offset are then delivered at their offset as usual. So the newest value
    always wins, and an older ring value can never overwrite the table.
  - The control thread seeds the table from the model before the host
    starts, so the host instantiates with the right values and reports the
    right latency (a look-ahead parameter changes it).
  - On the **first submit to a new channel** the audio thread copies its
    private store into the region and bumps `values_seq`. That covers every
    change made between seeding and the swap. Conditions (auditor A2):
    1. It copies the chain's actual `param_slots` entries, not the
       `kMaxParamsPerChain` cap.
    2. The copies are capped **per callback**, at 64 KiB in total
       (placeholder, about 16 384 values). A channel whose copy would pass
       the cap waits one callback before its first submit; it plays
       silence either way, so nothing audible changes. Without the cap a
       session open bringing up 64 chains would copy 2 MiB in one callback.
    3. Each `last_value` element is written with a relaxed atomic store,
       never `memcpy`, and `values_seq` is bumped with release after them.
    4. The destination is faulted in before the swap: the control thread
       pre-touches the region (or maps it `MAP_POPULATE`) whatever the
       `mlockall` state, because `MCL_FUTURE` only covers it if the lock
       succeeded.
    5. The private store is a fixed array of stores allocated at session
       creation; the plan node carries its **index** (§7.1). The audio
       thread never looks a store up by chain id.

**5.2 Parameter values are format-native, not format-neutral.**

- `value` is the LV2 port value (plain, within the port's range) for an LV2
  instance and the VST3 **normalised** value (0..1) for a VST3 instance.
  The session file stores the same (§6). The engine only clamps, to the
  range in the descriptor or to [0, 1].
- Why: a format-neutral value needs a conversion somewhere. VST3's
  plain↔normalised conversion is a controller call that must run off the
  processing thread (`plugin-host-vst3.md`, finding 8), and LV2 has no
  normalised form at all. Native values put no conversion on any RT path
  and no thread hop between an automation point and the plugin.
- Display text comes from the host (`DescribeValue`) or, for our own
  plugins, from the descriptor (ADR-0011 §2.4). The UI draws a
  normalised-looking control from the descriptor's range either way.
- Automation (DAW 4) interpolates in the stored value's space: VST3 in
  normalised space, as VST3 hosts do; LV2 in plain space, or log space for
  a port marked logarithmic.

**5.3 Crash and restart: a region is never reused.**

- A region a dead or killed host touched is unmapped and closed, never
  reused. It is released only after the plan that referenced its channel
  has been retired by ADR-0009 §4.1's epoch rule, because the audio thread
  may still be reading it.
- **Restart** = new region memfd, new clock-region mapping, new host, wait
  for `Ready` (on `done_word` and the socket, from the control thread,
  bounded at 10 s), re-read `latency_frames` through `checked_latency`,
  build a plan with the new channel and swap it in. Parameters come from
  the private store (§5.1); state from the last `State` the engine holds
  (taken at the last save, or none).
- **Policy:** automatic, at most 3 restarts of a chain in 60 s. After that
  the chain stays down (its path silent), and the operator gets the plugin
  named with three actions: retry, make the insert inactive (§6.3), or
  remove it. A restart is allowed while recording: capture is pre-insert,
  so it cannot touch the take.
- **Chain edits use the same path (make-before-break).** Adding, removing,
  moving, activating or deactivating an insert starts a new host with the
  new chain while the old one keeps playing. State for the instances that
  carry over comes from `GetState` on the old host. When the new one is
  `Ready`, a plan swap moves the node to the new channel, and the old host
  is shut down after the retire. One code path for crash, edit and load.
  Cost: a chain edit takes as long as the slowest plugin's load, and memory
  briefly doubles for that chain (Unverified 5).

**5.4 Plan swaps.**

- **`HostChannel` state lives outside the plan,** in a fixed table of
  channel slots that `engine/plugin_client` allocates at session creation
  (`kMaxChannels`, default 256). A plan swap that keeps the host keeps the
  slot, its block counters, its miss streak and its in-flight block:
  nothing resets. The per-chain private parameter store follows the
  chain's stable id through its index, so it survives a host replacement
  too.
- **A slot is retired by epoch, like a region (auditor H1).** A slot whose
  host has gone (crash, kill, make-before-break) returns to the free list
  only after **every plan that names it has been retired** by ADR-0009
  §4.1's epoch rule, the same rule §5.3 applies to regions. Until then a
  retired-but-still-running plan could name the slot index, and if the
  slot had already been handed to a new host the old plan would collect
  that host's output or submit into it. As a second guard, each slot
  carries a `generation` (`uint32_t`, bumped by the control thread when it
  assigns the slot to a new host, before publishing the plan that uses
  it), and the plan node carries the generation it was built against. The
  audio thread compares them in `collect` and `submit`; a mismatch is
  treated as a miss and counted as a fault, never as data. The free-list
  and the generation are written by the control thread only.
- **A swap that changes a node's channel** (a new host) discards the old
  channel's in-flight block. The first callback on the new channel submits
  and outputs silence; the next collects and ramps in (§3). One silent
  block per chain edit, plus the PDC realignment below. Priming the new
  host one block early for a gapless swap is a later refinement, only if
  listening says the blip matters.
- **Latency changes.** The host bumps `latency_generation`; the control
  thread notices within 50 ms, rebuilds the plan with the new figure and
  swaps it in. Until then the old alignment stands. The audio thread never
  reads `latency_frames` from a region: the plan carries the figure the
  builder used.
- **A "dry" miss does not exist** (§3), so the draft's rule about the dry
  signal delayed by `latency_frames` is withdrawn, not implemented.

**5.5 Versioning and incompatibility.**

- `kLayoutVersion` becomes **1** on acceptance. `header_bytes`
  (`sizeof(ChannelHeader)`) follows `layout_version`; the clock region gets
  its own magic and `kClockLayoutVersion` 1; `host_control.h` its own
  `kHostWireVersion` 1.
- A contract test holds `static_assert`s on `sizeof` and `offsetof` of
  every field of `ChannelHeader`, `ParamChange`, `InstanceReport` and the
  clock header. Any layout change fails the build until the version and the
  asserts move together, and `code-reviewer` checks that they did
  (ADR-0007 §6).
- **A host that does not speak the engine's versions** exits with code
  **3** (`kHostExitIncompatible`), distinct from usage errors (2), after
  one stderr line naming both versions, **before writing anything** to the
  region or sending anything on the socket. The engine reports it as an
  install fault, naming the binary.
- **Checked at start-up, not first use.** The engine runs
  `sonora-plugin-host --version` (layout, clock and wire versions) when it
  starts. A mismatch disables plugin hosting for the run and puts the
  reason in the state block's health flags in red; recording and playback
  without inserts still work. A session with inserts then opens with every
  insert shown faulted, never silently dry.

**5.6 The wake.** As §1: timer-paced hosts by default; one shared
`FUTEX_WAKE` per callback once Unverified 1 passes and an amendment turns
the gate on. The clock header's `wake_mode` tells the host which.

### 6. Inserts in the model and the session format

**6.1 The model.** `common/model` gains, per track (and later per aux, bus
and master, with the DAW 4 mixer):

```cpp
enum class PluginFormat : std::uint8_t { Lv2, Vst3 };
struct PluginRef {
    PluginFormat format;
    std::string id;        // LV2 URI, or the VST3 class id as 32 hex digits
    std::string name;      // as scanned; for the operator when it is missing
    std::string version;   // as scanned; diagnostic
};
struct ParamValue {
    std::string key;       // LV2 port symbol, or the VST3 ParamID in decimal
    float value;           // format-native (ADR-0012 §5.2)
};
struct StateRef {          // plugin state lives beside the session, not in it
    std::string file;      // "Plugin State/<sha256>.state"
    std::string sha256;
    std::uint64_t bytes = 0;
};
struct Insert {
    Id id;
    std::uint8_t slot;     // 0..9, A..J; unique on the track, gaps allowed
    PluginRef plugin;
    bool active = true;    // structure: an inactive insert is not in the chain
};
struct InsertMix {         // saved, not undoable (ADR-0009 §3)
    Id insert;
    bool bypass = false;
    std::vector<ParamValue> params;
    std::optional<StateRef> state;
};
```

- **Structure** (`Insert`: which plugin, which slot, active) is undoable,
  on `TrackEdit` (`EditState`). **Values** (`InsertMix`: bypass,
  parameters, state) are mix state on `MixState`, saved and not undoable,
  exactly as gain and mute (ADR-0009 §3). Where structure sits is owner
  decision O1; the file format does not depend on it.
- Parameter keys are the **stable** identifiers, never a dense index: an
  LV2 port symbol is stable across plugin versions, a port index is not; a
  VST3 ParamID is stable by the VST3 rules. The dense index is a run-time
  mapping built from the scan.
- **State is a file, content-addressed**, in `Plugin State/` inside the
  session folder, written durably by the saver thread
  (`write_file_durably`) before the session file that names it. Inline
  state would put megabytes into every Snapshot and undo window (64 MiB
  wire cap). Like sources, state files are append-only; unused ones are
  removed only by an explicit cleanup.
- **A missing plugin is kept, never dropped.** At open, an insert whose
  plugin is not in the catalog stays in the model with its values and
  state, is left out of the chain (as if inactive), and is shown as
  missing. A lenient loader that dropped it would delete it on the next
  save.

**6.2 Session format: version 2.** `session-format.md` is updated in this
PR:

- `sonora_session` goes from 1 to **2**. The `v1→v2` migration adds
  `"inserts": []` to every track, nothing else. A frozen v1 fixture must
  load through it forever. The Phase 3 mixer migration in the worked
  example becomes `v2→v3`. If another bump lands first, inserts take the
  next integer; the rule is one integer per change, not the number 2.
- Each track gains `inserts`, an array ordered by `slot`:

```json
"inserts": [
  {
    "id": 41,
    "slot": 0,
    "plugin": { "format": "lv2", "id": "http://gareus.org/oss/lv2/dpl#mono",
                "name": "x42-dpl Digital Peak Limiter Mono", "version": "0.9.2" },
    "active": true,
    "bypass": false,
    "params": [ { "key": "threshold", "value": -1.0 } ],
    "state": { "file": "Plugin State/3f1c…e9.state", "sha256": "3f1c…e9", "bytes": 812 }
  }
]
```

- Rules: `slot` 0–9 and unique on the track; ids from the shared id space;
  `params` keys unique per insert; `state` optional; `version` diagnostic
  only. A state file whose hash does not match is reported at open and the
  insert loads with defaults plus its `params`, shown as such. The parser
  refuses an unknown `format`.

**6.3 Operations** (`operations.h`, `op_codec.h`, additive public headers
of `common/model`, so contract files under ADR-0007 §6):

| Operation | Undoable | Effect |
|---|---|---|
| `AddInsert{track, insert, slot, plugin, params, state}` | yes | new insert in a free slot |
| `RemoveInsert{insert}` | yes | its `InsertMix` entry outlives it, like `TrackMix` after an undone `AddTrack`, so redo restores its values; pruned once no step in the undo window holds the insert |
| `MoveInsert{insert, to_track, to_slot}` | yes | between slots or tracks; refused onto an occupied slot, and onto a track of another channel count (a chain has its track's layout, §7.1) |
| `SetInsertActive{insert, active}` | yes | Pro Tools' active/inactive: in or out of the chain |
| `SetInsertBypass{insert, bypass}` | no | mix state |
| `SetInsertParams{insert, params}` | no | the end of a gesture, and every insert's values at save |
| `SetInsertState{insert, state}` | no | at save, after `GetState` |

Ids are allocated as for every other operation today; moving `resolve_*`
into the engine (ADR-0007 M7 note item 6) removes the same race here.

### 7. Plugin chains and PDC in the playback plan

**7.1 Node kinds.** `NodeKind` (append-only) gains three kinds. A track
with no active insert keeps today's `TrackOut`, which fuses all three.

| Kind | Does | Fields it uses |
|---|---|---|
| `TrackSource` | steps 1–2 of the monitor-bus plan: input or playback, region gain and fades, at the track's channel count (1 or 2), no fader, no pan | `track` |
| `InsertChain` | collects the chain's previous output from its channel, submits this block's input (§7.2) | `channel_slot`, `channel_generation`, `param_store`, `latency_frames`, `channels`, `chain_id` |
| `TrackFader` | step 3: the track's gain, mute, solo and pan from the `Track` atomics, ramped per block, into stereo | `track` |

- A track with inserts is `TrackSource → InsertChain → TrackFader →
  BusSum`. **Inserts are pre-fader**, as in Pro Tools and Logic, so a fader
  move never changes what a compressor sees.
- `PlanNode` gains `channel_slot` (the `HostChannel` slot, §5.4),
  `channel_generation` (the slot generation it was built against, §5.4),
  `param_store` (the index of the chain's private parameter store, §5.1),
  `chain_id` (the chain's stable id: the track id, so the control thread
  can keep the store with the track; never looked up on the audio thread),
  `latency_frames` (the chain's checked
  figure, IPC block included, as read when the plan was built) and
  `channels`. Stable ids: `TrackSource` and `TrackFader` derive from the
  track id like `TrackOut` (`layout_monitor_graph`), `InsertChain` from
  the chain id.
- The chain's channel count equals the track's, in and out. A plugin that
  cannot map to it is refused at validation (`ChannelMismatch`). Mono-in
  stereo-out inserts (a stereo reverb on a mono track) are a DAW 4/5
  item with their own node rule.
- Capture is untouched: the capture path reads the backend's inputs
  before any node runs. No node added here is on it.

**7.2 Execution on the audio thread.** The node list stays serial and in
topological order. An `InsertChain` node, per protocol:

0. If the slot's `generation` differs from the node's
   `channel_generation`, the node is a miss (faded out, §3) and submits
   nothing.
1. If its channel's `completed_block` is the block submitted last callback
   (depth 1), copy the output planes into its scratch buffer (ramped in
   after a miss); otherwise the §3 fade-out then silence, counted.
2. If the host is idle (`completed_block == submitted_block`), write its
   input from the upstream node, push any pending parameter entries (on a
   first submit, the store copy of §5.1 within the per-callback byte cap),
   publish `submitted_block`; otherwise skip the submit and count it.
3. After the plan's last `InsertChain` node (the builder records its index
   as `last_insert_node`), `tick.fetch_add(1, release)` if anything was
   submitted, and, only with `kHostWakeEnabled`, one `wake_all()`.

Nothing here waits, allocates or reads a pointer from the region; plane
addresses come from the engine's private `Layout`.

**7.3 PDC: the builder sets `delay_frames`.**

- Over the topological order, the builder computes for each node its
  output latency `L(n) = max over input edges of L(from) + d(edge) +
  latency_frames(n)` (0 for every non-chain node), and for each input edge
  of a node `d(edge) = max_in − L(from)`, where `max_in` is the largest
  `L(from)` among that node's inputs. Every path into a sum arrives aligned
  on the slowest one. The result is sample-exact or it is wrong
  (`plugin-latency-compensation.md`).
- `delay_line` stays the same for the same `(from stable id, to stable id)`
  pair across plans, as #41 already does for its edges, so a swap never
  restarts a delay line from silence.
- **Maximum compensation** is a session-creation maximum,
  `max_delay_frames` (the field #41 already has), default **16 384**
  frames (0.34 s at 48 kHz). Memory: edges × 2 channels × 16 384 × 4 bytes,
  32 MiB at 128 delayed edges, preallocated and locked.
- **The delay-line pool is capped by edge count too (auditor L3).** The
  pool is sized at session creation for `max_delayed_edges` (default 128)
  lines. The builder, on the control thread, assigns `delay_line` indices
  only below that bound. A plan needing more delayed edges is built with
  the excess edges undelayed (aligned to nothing, so audibly early),
  flagged in red with the count, by the same reasoning as the clamp below.
  The audio thread never indexes past the pool: a `delay_line` index is
  valid by construction, and `Session::process()` asserts it in debug
  builds only.
- **Over the maximum, the plan is built, not refused, and the overrun is
  reported.** The edge is clamped to the maximum and the track is flagged
  in red in the state block with the shortfall in frames. This departs from
  ADR-0009 §4.5's "refuses a plan that exceeds one" for this one maximum,
  because the latency is learned from the host after the edit was accepted:
  refusing would leave the engine playing a plan the document no longer
  describes. A control that lies is worse; a red figure does not lie.
- The total compensation (`L` at `HardwareOut`) goes in the state block, and
  each track's chain latency already has `TrackState::latency_frames`
  ("reported honestly").
- The plan carries the latency it was built with; nothing on the audio
  thread recomputes alignment (§5.4).

### 8. Insert operations through the control API (ADR-0007 §6)

All of it is **additive** under §6: new message kinds, new enum values,
appended state-block fields with the version bumped. Each PR needs every
owning party's review (ui-engineer and rt-engineer for `contracts/api` and
`common/model`; plugin-engineer and rt-engineer for `contracts/plugin`) and
updates `FakeEngine` and the conformance suite in the same PR.

- **Structure and values that are saved:** `ModelRequest{Apply}` carrying
  the §6.3 operations, through the existing model path. The control thread
  applies them to the document, then (§5.3) starts the new host, and the
  plan swap follows when it is `Ready`. The reply comes when the document
  changes; audibility follows. A refusal (no such plugin, slot taken,
  validation refused) is a reply with the reason, and for a `Refused` host
  also a `PluginFault` event naming the plugin.
- **Continuous parameter changes:** a new message,
  `PluginParameterChange{seq, insert, key, value}`. The control thread
  resolves the stable key against the running instance's own key table
  (built by the scan that instantiated it), maps it to the dense index of
  §5.1 and sends it on as an `RtCommand`. Not the catalog's dense index:
  a rescan can reorder a plugin's parameters while a drag is in flight,
  and an index from the old catalog would move another parameter
  (rt-engineer review of #46; a catalog generation in the message was the
  other option, and would only have refused the drag). `ParameterChange`
  stays track-only. The UI writes the end of a gesture with
  `SetInsertParams`, as faders will with `SetMix`.
- **Values moved in the plugin's own editor** (ui-engineer review of #46,
  H1): the host's `ParamTouched` comes to the engine, not to a client, so
  the **engine** records them. At the end of each `ParamTouched` gesture,
  and on `EditorClosed` for whatever moved without a gesture end, it
  applies `SetInsertParams` (the insert's whole list, the moved keys set)
  to the document and every replica gets it as a `ModelUpdate`. Nothing is
  sent while a gesture runs; a client is at most one gesture behind, and
  the generic panel, compare and the saved values never hold a stale value.
  Option (b), a `PluginParameterValue` engine → client message, was not
  needed for that and is not in protocol 3.
- **Flushing plugin state** (H1's companion, M1): `ModelAction::FlushPluginState`,
  `op` empty for every insert or one insert id in decimal. The engine has
  each running plugin's state written (`GetState`, then the saver thread's
  durable write of §6.1), applies `SetInsertState`, and `SetInsertParams`
  for editor values not yet recorded; the reply comes after the last of
  those updates, so a client whose reply is Ok holds what a session file
  must. File > Save, and an insert copy or compare on a state-heavy plugin,
  send it first. Failed names each insert whose state could not be had;
  the others' updates still went out.
- **The catalog:** a new engine → client message, `PluginCatalog`: per
  plugin, format, id, name, vendor, version, categories (LV2 class labels
  or VST3 subCategories, broadest first), channels, usable or the refusal
  reason, has-editor, and per parameter its dense index (display order
  only), key, name, range, default, flags (automatable, toggle, enum,
  logarithmic, past the cap), unit, step count (0 continuous) and one label
  per step when the plugin names them. Sent after the Snapshot and again
  after a rescan.
- **Commands** (catalog ids, `Engine` domain, key-mappable):
  `plugin.rescan`; `plugin.editor.open` and `plugin.editor.close` with the
  track as target and `"<slot> <parent token>"` as argument (the token
  is opaque and empty on the appliance); `plugin.retry` for a chain left
  down by §5.3.
- **Events:** `EventKind::PluginFault` (append-only): crashed, removed for
  misses, restarted, refused, non-finite output, missing, host
  incompatible. It names the insert by its model id in a field of its own,
  `EngineEvent::insert` (not `track` and slot, which a `MoveInsert` or
  `ReorderTracks` in flight would make wrong; not `frame`, which is the
  take number). `track` is -1; the text names plugin, track and slot for a
  log. One fault **stands** per insert: an active insert that is down or
  missing has one, the latest replacing the last, and leaving that state
  (running again, removed, undone, made inactive, another session opened)
  clears it with an empty text. The control server keeps the standing
  faults by insert id, apart from the 32 standing notices, and replays
  only those to a client that attaches later. Recomputed from the
  document after every change, so an insert that undo or `SetInsertActive`
  brings back into the chain is reported as one a `Load` brings.
- **State block, layout version 2:** per track an `InsertState` for each of
  the 10 slots: flags (present, active, bypassed, running, restarting,
  faulted, missing) and latency frames under the seqlock; misses, worst run
  in µs and non-finite samples (all monotonic while the slot holds the same
  insert) **outside** it, each a word read alone. Once hosts run those
  counters move on most publishes, and inside the section they made every
  slot a store in it again and starved a reader (rt-engineer review of
  #46; the regression test churns them on every publish under TSan). In
  the header the total compensation and the over-maximum shortfall. If PR
  #41's planned layout-2 fields (locate, bus peaks) land first, this is
  layout 3.
- **Telemetry for our own editors** (ADR-0011 §2.6): one more memfd per
  host, created by the engine, mapped read-write by the host's RT thread
  (atomics only, paced at most once per block), forwarded to a client on
  request with `SCM_RIGHTS` and mapped read-only, like the state block.
  Per instance: a fixed array of meter slots, each one 8-byte atomic packing
  a sequence and a float (the state block's peak-ring rule), and a lossy
  sample tap with a write counter. Exact layout in its own
  `contracts/plugin` PR with N1; this ADR fixes only that shape.
- **Protocol version 3** for the new messages; `Hello` mismatch refuses by
  name, as today.

**Amendment, 2026-10-14: the control server's chains (PROPOSED, checklist
5).** Proposed by plugin-engineer. The architect ruled on #64: accept
with changes A to H, applied below; the rt-engineer's view is in the same
ruling. Still Proposed until the owner question at the end of item 1 is
answered. It settles what this ADR leaves open when §5.3 and §8 are wired
into `engine/control`. It **changes accepted text in three places**: the
ADR-0007 module graph (item 4), §8's list of `PluginFault` causes (item 5),
and the live-period assumption of the first draft of item 4, which ADR-0007
§4.2 never allowed. It builds on the rule #59 and #68 put in
`engine/core`: an unbound chain is laid out with latency 0 and passes
through, and a bound chain that is not run plays silence.

1. **What a chain plays while it has no Ready host.** One rule: *until its
   host is Ready, a chain plays what the playing plan already played for
   it.* After an edit that is the old host; for a chain the playing plan
   did not have, it is dry (#59's unbound chain, latency 0); when there is
   nothing to carry over (session open, a crash, down), it is silence.
   Inserting a limiter never changes what is already in the monitors, and
   opening a session never plays a mix the session never had. The resolver
   binds an active chain as follows:
   - *Running:* its current host's slot, generation, store, node state and
     checked latency.
   - *Edited while running* (an insert added, removed, moved, activated,
     deactivated or replaced on a chain whose host is Ready): the old
     host's binding, unchanged, until the new host is Ready (§5.3,
     make-before-break). The document leads the audio by the load time.
     The resolver diffs against the document on every change: a start
     whose spec the document no longer describes (an undo before Ready, a
     second edit) is abandoned and its host killed, while the old binding
     keeps playing; the newest spec starts instead (change H).
   - *Edited while silent* (restarting or down): stays silent, as below,
     until the new host is Ready.
   - *New on a track that had no chain in the playing plan* (its first
     active insert, by Apply, undo or redo): unbound, latency 0. The track
     plays dry and aligned, as it did before the insert existed. The
     operator hears what was already playing, not a fault.
   - *Restarting* (after a crash, a removal or a hang): the dead host's
     binding stays named, with its checked latency, until the
     replacement's `Swapped` (as `SupervisedChain` does, #72). The client
     plays silence for it (§3).
   - *Down* (three restarts in 60 s, or a start that crashed or timed out):
     **silent**, through a new constant, `kSilentChannelSlot` (0xFFFFFFFE)
     in `playback_plan.h`, laid out with the **last checked latency** of the
     host it replaces (change A), so the dead slot can be retired without
     moving any other path. `Session::process` writes silence for it
     without calling the runner and drops its parameter events, counted.
   - *Starting with the session* (start-up, `ModelAction::Load`) and
     *hosting disabled* (§5.5): silent, through `kSilentChannelSlot` with
     latency 0, since nothing is known yet. Never dry: after a limiter or a
     cut, dry is louder (Rationale), and a session that opens must not play
     its master unlimited for the seconds its hosts take to start.
   - A silent chain therefore realigns nothing on a fault. The only things
     that realign the mix after a fault are operator actions: make
     inactive, remove, retry.
   - *Missing* plugins are left out of their chain (§6.1, "as if
     inactive"), and the chain runs with the others. A chain whose
     instances are **all** missing is no chain (§2): its track plays dry,
     with a standing "not installed" fault on each insert (change B), as
     Pro Tools and Logic do with a missing plug-in.
   - *Refused* instances (`Refused` at validation, a probe that crashed or
     timed out) are treated as missing (change C): left out, with their
     standing fault, and the chain starts without them; during an edit the
     old binding keeps playing until that start is Ready. A refused
     instance stays out until the document changes or the operator chooses
     `plugin.retry`, so this cannot loop. A crash or a timeout of a host
     during start keeps the restart policy (§5.3).
   - **Play at session open: decided by the owner, 2026-10-18.** Play
     from a freshly opened session waits until every starting chain is
     Ready or has failed (down, or its instances refused or missing), for
     at most §5.3's 10 s, and the interface shows "Loading plug-ins (n of
     m)" meanwhile, as Pro Tools does. Past the bound, play starts and a
     chain still starting stays silent until it arrives. Record never
     waits: capture is pre-insert, and a take is worth more than
     monitoring through inserts.

2. **Retiring a slot by epoch, concretely (§5.4, H1).**
   `PlaybackController` reports `oldest_live_epoch()`: the smallest epoch
   among the playing plan, the pending plan and every retired plan not yet
   freed. A slot, with its region and its host, leaves its chain when the
   resolver stops naming it. The chain records the epoch of the first plan
   built without the slot. It calls `PluginClient::retire(slot)` and
   `SupervisedChain::retired()` only once `oldest_live_epoch()` is at or
   past that epoch, that is, once no live plan is older than the first plan
   built without the slot. A cancelled plan is never read by the audio thread, so it
   holds no slot. The generation check (H1) remains the second guard.

3. **Where a chain's spec comes from until the scan worker and catalog
   cache exist.** A chain start is asynchronous from end to end
   (code-reviewer L6 on #49):
   - For each plugin not yet probed in this engine run, one
     `sonora-plugin-host --probe ID` child is started (one plugin per
     process, as the host's probe mode is built; §4 says one per bundle,
     and the scan worker settles which). The control thread collects it from its poll and never
     waits on it. Probes are deduplicated in flight (two chains waiting
     on one plugin share one probe), run concurrently up to a cap
     (placeholder: the hardware threads), and each has a timeout. A probe
     that crashes or times out marks the plugin refused for the run (§4's
     "blocked"; item 1, change C). The cost is unmeasured: Unverified 11.
   - Then the host starts with `--control-fd` and receives its chain in
     `Load` (`host_control.h`), not on the command line. The key table
     against which the engine resolves `PluginParameterChange{insert, key}`
     is then the running instance's own, from `Loaded` (§8). `GetState` is
     available for `FlushPluginState` and make-before-break.
   - Probe results are kept for this run only. The persistent cache, keyed
     by bundle path, size and mtime, belongs to the scan worker, in a later
     PR.
   - **Parameter stores (change F).** A restart keeps the chain's store:
     the dense layout is the same, so §5.1's "survives a host replacement"
     holds. An edit is a new `SupervisedChain` with its own store and node
     state, seeded from the document: during make-before-break the old host
     still plays from the old layout, and the new chain's dense indices
     differ (an insert added in slot A shifts every index), so a shared
     store would let the old node write old indices into what the new
     host's first submit copies. The old store is retired by epoch with
     the old slot (item 2). A `PluginParameterChange` during the overlap
     resolves against the **old** host's `Loaded` table; the value reaches
     the new chain through the document (`SetInsertParams` at gesture end)
     or the new store's seed.

4. **One owner, one backend activation.** The plugin side is one object,
   `PluginHosting` in `engine/control`, on the document thread beside
   `PlaybackController`, with one poll set and no new thread or lock. It
   holds `PluginClient`, `HostSupervisor`, `PluginChainRunner` and every
   `SupervisedChain`.
   - It lives for **one backend activation** (change D). It is built once
     `buffer_frames()` is non-zero: `block_frames` is that period, and
     `max_chain_channels` is 2. ADR-0007 §4.2 makes a period of another
     size fatal: the backend goes `fatal()`, the engine closes the take and
     says why, and `deactivate()` is the barrier. There is no live period
     change to rebuild for. A reopen at a new period builds a new
     `PluginHosting` after `deactivate()` has returned, and frees
     everything retired at once (ADR-0009 §4.1). Live buffer-size changes
     would be an ADR-0007 amendment first (hosts activated for a maximum
     block, and a contract change to how the region's IPC latency is
     reported); Pro Tools does not offer one during a take either.
   - It owns the teardown order: the runner leaves the session; then
     `blocks_` has moved past the value read at that exchange, or
     `deactivate()` has returned (never a timeout: a stalled backend does
     not move `blocks_`); the chains stop once the plans naming their
     slots are retired; then the supervisor goes; then the client.
   - **It amends the ADR-0007 module graph (change E):** a direct edge
     `engine/control → engine/plugin_client`, restricted to the
     control-thread half (`host_supervisor.h` and `PluginClient`'s control
     API). The RT half is reached only through `engine/core`'s
     `PluginChainRunner`. ADR-0012 §4 and checklist 4 already put the
     supervisor on the control thread, and ADR-0009 §4 puts the resolver
     and plan building in `engine/control`; today the edge would compile
     only transitively, through `engine/core`'s public link, which the DAG
     forbids ("anything not drawn is forbidden").

5. **`PluginFault` comes from the client's per-slot status** (rt-engineer
   item 2 on #59). One fault stands for each insert of an active chain that
   is not running. Its text names the plugin, the track and the slot.
   - *Starting* (session open): no fault; the state block's `restarting`
     flag shows it. A start that crashes or times out makes the chain
     *down*.
   - *Restarting:* "restarting: <why>" on every insert of the chain, with
     the culprit named (finding 6). Cleared at `Ready`.
   - *Down:* the culprit gets "<plugin>: <why>; 3 restarts in 60 s, the
     chain stays down (retry, make inactive or remove)". The other inserts
     of the chain get "down with <culprit>".
   - *Refused:* the refusal text, on the instance that `Refused` names; the
     chain runs without it (item 1, change C).
   - *Missing:* "not installed", on that insert alone.
   - *Hosting disabled:* as today, on every active insert.
   - **Non-finite output (change G, amends §8).** §8 lists non-finite output
     as a `PluginFault`. It is not a standing one: the host scrubs it and
     the chain plays on, so a fault that stood would describe a chain that
     is running. Instead, the first non-finite block an insert produces in
     a run posts one **non-standing** `Notice` naming the plugin, track and
     slot, and the state block's `InsertState` counter carries the count
     from then on. The operator is told once; nothing stands.

## Rationale

- **What each choice costs later.**
  - *One broadcast wake* keeps the audio thread's syscall count at one,
    whatever the number of chains grows to. Per-host wakes would put 18 to
    64 syscalls in a callback at full mixes, and that number would only
    grow. Its later cost is that on the RT kernel it can sleep on a
    contended, PI-bounded hash-bucket lock (§1.1), which is why it ships
    gated off: the timer path costs dropped blocks we can count, the wake
    costs an unbounded-until-measured stall in the one thread that must not
    stall. If the measurement fails, §1.3 stays, and the contract does not
    move.
  - *Host per chain* keeps a crash to one track, gets parallelism without
    writing a scheduler, and matches "once per block per host process"
    (ADR-0007 §4.3), which per-plugin hosting would turn into once per
    plugin. Its later cost is locked memory per process, which is
    measurable (Unverified 3); a host carrying several chains is a later
    protocol revision if the figure demands it, because a region is per
    chain and a host could map more than one.
  - *Silence on a miss* is safe at any gain. Dry delayed by latency is
    louder after a limiter and after any plugin set to cut (ADR-0011 §1.6
    named the limiter), so a stall on a master chain could put a level jump
    into monitors. It also hides the fault, needs a delay line per chain up
    to that chain's latency, and is the more complex path for a rare event.
  - *Format-native values* put no conversion and no extra thread on the
    path from an automation point to a plugin.
  - *Make-before-break* means one code path for load, edit and crash, and a
    chain edit during playback never stops what is playing.
- **The hard constraints.** No mutex, no wait and no allocation is added to
  the callback. The one syscall can block briefly on the RT kernel, so it
  is off until bare metal proves its bound. Capture is
  pre-insert in every case. Version mismatch is found when the engine
  starts, not when the operator inserts the first plugin.
- **Prior art for a wake from the RT thread (recalled, not verified in this
  repository).** JACK2 activates clients from its RT thread through futex
  or semaphore posts, and PipeWire's graph signals the next node from its
  data loop with an eventfd write. If that holds, our audio thread runs
  inside a server that already makes this kind of call every period. The
  researcher confirms it from the sources (Unverified 7). Prior art does
  not replace the measurement: the gate follows Unverified 1 only.

## Consequences

- **ADR-0007 §2's table changes (this is the amendment §6 requires for
  moving code into the RT boundary):** `common/platform` may contribute
  exactly `wake_all()` to the audio thread, under the auditor's A3
  conditions:
  - it is the **only** `common/platform` symbol marked `SONORA_RT`;
  - it is `noexcept` and calls raw `syscall(SYS_futex, ...)`: no logging,
    no allocation, nothing on its failure path, errno ignored;
  - a test fails the build if any other `common/platform` symbol is
    reachable from `Session::process` (an `nm`/link check on
    `engine/core` and `engine/plugin_client`);
  - the RTSan job either allows it with a scoped suppression around that
    one call or reports it as the one known syscall. Whether RTSan
    intercepts `syscall()` at all is unknown (Unverified 10); a one-line
    RTSan test calling `wake_all()` from a `[[clang::nonblocking]]`
    function settles it, and it is written even while the gate is off.
- ADR-0007 §4.3's sketch is superseded where it differs: no `deadline_ns`
  on `collect` (nothing waits); `HostChannel` has `collect(out) →
  Collected | Missed | Removed` and `submit(in, frames)`, both `SONORA_RT`.
- `plugins/host` changes with the contract: it sleeps on the clock
  region's `tick`, applies tagged parameters and the table, implements
  bypass, speaks `host_control.h` on fd 3, and exits 3 on a version
  mismatch. Its test driver plays the engine's new role.
- **Latency on monitoring through inserts** grows by one block per serial
  insert stage (§2). The mixer must show it, and DAW 4's low-latency
  monitoring (input without inserts on armed tracks) is the operator's
  remedy.
- Locked memory grows by one host process per chain with inserts.
- A chain edit is audible as one silent block plus any realignment.
  **The realignment is larger than it reads** (amendment of 2026-10-14):
  when a chain becomes Ready mid-play its latency goes from 0 (or the old
  host's figure) to L, the builder changes `delay_frames` on every other
  path into that sum, and because each edge keeps its `delay_line`, the
  read point steps: about L frames of the rest of the mix repeat (or are
  skipped, when L falls). Whether a short crossfade at a delay change is
  needed joins Unverified 6's listening check. A fault never causes one:
  restarting and down chains keep their last checked latency (item 1,
  change A); only operator actions realign.
- ADR-0009 §4.5's "refuse a plan over a maximum" has one exception
  (§7.3).
- The session format moves to version 2, and every file written by this
  build is unreadable by an older one, loudly (`session-format.md`,
  "Versioning").

## Rulings of the rt-safety-auditor

Ruled on PR #42 (comment 6149365704) against `e62eff6`. Where each is
applied:

| Item | Ruling | Applied in |
|---|---|---|
| A1, one shared `FUTEX_WAKE` per callback | Accepted with conditions, **provisional**: gate off, timer-paced hosts by default, on only after the bare-metal measurement; skip when nothing submitted, never retry, ignore errno; rt-kernel hash-bucket-lock caveat recorded | §1.1, §1.3, §1.4, §7.2 step 3, Unverified 1, checklist 7 |
| A2, first-submit store copy | Accepted with conditions: `param_slots` only, 64 KiB per callback cap, atomic stores, control-thread pre-touch, store by index from the plan node | §5.1, §7.1, §7.2 step 2 |
| A3, `common/platform` on the RT path | Accepted with conditions: `wake_all()` the only `SONORA_RT` symbol, `noexcept`, raw syscall, no logging; reachability test; RTSan check | Consequences, Unverified 10, checklist 4 |
| H1, slot reuse | Slots retired by plan epoch, per-slot generation checked on the audio thread | §5.4, §7.1, §7.2 step 0 |
| M1, read-only clock region | Sent as the `O_RDONLY` reopen with `F_SEAL_FUTURE_WRITE`, as `ipc.cpp:179-186` | §1.1 |
| M2, lost-wakeup order | Host loads `tick` before `submitted_block`, rule in `protocol.h`, as `main.cpp:390-396` | §1.1, §1.3, checklist 1 |
| M3, two-writer `values_dirty` | Replaced by the engine-only `values_seq` counter | §5.1, checklist 1 |
| L1, table vs ring precedence | Ring entries ≤ N first, then the table; newest wins | §5.1 |
| L2, clocksource | vDSO only on `tsc`; reuse a timestamp or report the source | §1.1, Unverified 9 |
| L3, delay-line pool | Capped by `max_delayed_edges` at plan build; audio thread never indexes past it | §7.3 |
| L4, hard cut on a miss | 64-frame fade-out of the last sample; holdback rejected | §3, Alternatives |

Still the auditor's on every diff: every host-written word checked before
use; plane addresses from the private `Layout`; the masked parameter ring.
The general claims (depth 1 never waits, miss handling, SPSC ring,
regions never reused, swap discards the in-flight block) were found sound.

## For the owner

Neither blocks the contract or `engine/plugin_client`. Each has a default
the work proceeds on until the owner rules.

- **O1. Is inserting, removing or moving a plugin undoable?** Logic undoes
  mixer edits; Pro Tools does not undo most of them. **Default: undoable**
  (structure only; parameter values and bypass are not, like faders).
  ADR-0011's mix assistant needs it (its "compound operation that undoes as
  one"). Flipping it moves `Insert` from `EditState` to `MixState`; the
  session file is the same either way.
- **O2. Keys while a third-party plugin editor has focus** (left to this
  ADR by ADR-0010 §5). The compositor cannot route a key to an unfocused
  client. **Default:** keys go to the editor while it is focused, and the
  space bar does not start the transport until the operator clicks back
  into Sonora; the UI shows which window has the keyboard. The
  alternative, a host-side key forwarder that sends transport keys to the
  engine, is work in `plugins/host` per editor toolkit. This needs settling
  before editors ship (BACKLOG Track 4 item 5), not before inserts do.

## Unverified

Each has the measurement that settles it and who takes it.

1. **The wake's cost in the callback, bare metal, `rt-amd64` kernel.**
   The cost of the wake syscall alone, on the waker's side, has never been
   measured (`adr-0007-measurements.md`, "Still unverified" 2); the round
   trips we have bound nothing about it. Measure p50, p99.9 and max of
   `wake_all()` inside the callback with 1, 18 and 64 waiting hosts, at
   quantum 128, for at least 1 h, while unrelated processes run
   `stress-ng --futex` so that bucket contention is in the run; report
   separately the case where every host is in a wait at once (the worst
   O(N) walk); compare with 18 per-host wakes; and give the dropped-block
   rate under 18 chains with the gate on and off. **Pass: max under about
   130 µs (5 % of the period) at 18 and 64.** Pass turns the gate on by
   amendment; fail makes §1.3 the decision. Also informs depth 0.
   The run also counts the **hosts' own per-block `FUTEX_WAKE` on
   `done_word`** (block protocol step e, auditor L4 on PR #45): off the
   audio thread, but with N hosts it is N bucket-lock acquisitions every
   period on `PREEMPT_RT`, in the same hash table as the audio thread's
   wake. Measure the audio thread's wake with it on, and with hosts waking
   `done_word` only on a state change (the harnesses then poll); if the
   difference shows at max, the contract drops the per-block wake.
   Researcher, with the "ADR-0007 IPC figures on the RT kernel" BACKLOG
   item.
2. **`FUTEX_WAIT` on a sealed read-only shared memfd mapping.** Expected
   to work (the kernel takes the futex key without write access for a
   wait), not checked. A `platform_ipc`-style test in the container settles
   it. rt-engineer, in the contract PR.
3. **Locked memory per host** (`mlockall` with lilv and the plugin
   libraries loaded) for eg-amp, x42-dpl, an LSP plugin and a VST3: RSS
   and locked kB, so the number of chains a 4 GB and an 8 GB machine can
   carry is known. Container is fine. Researcher.
4. **Scheduling 18 hosts at FIFO 70 on 4 cores** at quantum 128 with
   realistic chains (four LSP plugins per track): dropped blocks per hour.
   Bare metal. Researcher.
5. **Make-before-break time:** from `AddInsert` applied to the plan swap,
   for a chain of five plugins, p50 and max. Container. plugin-engineer.
6. **Silence with a 64-frame fade-out and a ramp-in on a miss** is
   acceptable by ear compared with the alternatives. A listening check on
   forced misses (the `sleep` test plugin). Owner's ears, when a build
   plays.
7. **JACK2 and PipeWire wake syscalls from their RT threads** (Rationale).
   Researcher, from the sources.
8. **Default compensation maximum 16 384** is a placeholder that matches
   what Pro Tools operators expect; researcher to confirm the figure
   Avid documents for its delay compensation limit.
9. **The target box's clocksource is `tsc`**, so clock reads elsewhere in
   the engine are vDSO reads and not syscalls (`callback_ns` no longer
   needs it: since #68 the backend stamps it, §1.1). Read
   `/sys/devices/system/clocksource/clocksource0/current_clocksource` on
   the bare-metal machine; the start-up check of §1.1 covers boxes we have
   never seen. Researcher, with Unverified 1.
10. **Whether RTSan intercepts `syscall()`.** A one-line RTSan test calling
    `wake_all()` from a `[[clang::nonblocking]]` function. Decides between
    a scoped suppression and a reported known syscall (Consequences).
    rt-engineer, in the `engine/plugin_client` PR.
11. **Probe cost and session-open time** (amendment of 2026-10-14, item
    3). Every probe process and every serving host calls
    `lilv_world_load_all()` (`plugins/host/src/lv2_host.cpp`), parsing every
    installed LV2 bundle's TTL, so a session open costs K distinct probes
    plus C hosts, each with a full world load, before the 10 s Ready bound
    starts. Measure, in the container: probe wall time for eg-amp, x42-dpl,
    one LSP plugin and one VST3 with Debian's `lsp-plugins-lv2` and x42
    installed; then open-to-all-Ready for 18 chains × 4 plugins, cold and
    warm page cache, probes serial against capped-parallel. If an open
    passes a few seconds, the fix is the scan worker's persistent cache and
    `lilv_world_load_bundle` for the one bundle in probe and host mode, not
    more parallelism. plugin-engineer.

## Alternatives rejected

- **Wake: one wake per host.** Correct and simple, but *N* syscalls per
  callback, growing with the mix. Kept only as the comparison in
  Unverified 1.
- **Wake: eventfd.** Same cost per call as a futex (measured), but no clean
  broadcast, so it means one write per host, and one fd per host.
- **Wake: hosts spin.** Measured into RT throttling (46–51 ms stalls); a
  core per host. Making it work needs `sched_rt_runtime_us=-1`, which
  removes the kernel's protection against a runaway RT thread.
- **Wake: timer-paced hosts as the permanent choice.** No syscall on the
  audio thread, but hosts wake on a guess and lose the timer jitter from
  every period. It is the default until the wake is measured (§1.3), and
  the decision only if the measurement fails.
- **Wake: the broadcast wake on by default, measured later.** Rejected by
  the auditor's condition: on `PREEMPT_RT` the wake can sleep on a bucket
  lock any process can hold, and nothing bounds it yet. A stall we cannot
  bound in the callback is worse than dropped blocks we can count.
- **Parameters: a `values_dirty` flag the host clears.** Two writers on
  one word; a set landing while the host reads the table is lost (M3).
  The engine-only `values_seq` counter has one writer.
- **Miss onset: hold back a short tail of every block to fade real
  audio.** Adds the tail to every chain's latency all the time, for a
  fault event. The fade of the last sample (§3) costs no latency.
- **Slots reused as soon as their host dies.** A retired plan still
  running could submit into the next host's slot (H1).
- **Granularity: one host per plugin.** Smallest blast radius, but the
  boundary crossed once per plugin (72 serial round trips at 18 × 4,
  roughly 60 µs each at p50), which ADR-0007 §4.3 forbids.
- **Granularity: one host for the session.** Fewest processes, but one
  crash silences every insert, and parallelism would need a worker pool
  inside the host: the scheduler we would rather not write, in the process
  we trust least.
- **Miss: dry delayed by latency.** Louder after a limiter or a cut,
  hides the fault, and needs a delay line per chain. See Rationale.
- **Miss: repeat the last block.** A buzz at the block rate during a
  stall, and it plays audio that is not the music at that time.
- **Miss: wait a little.** Forbidden by ADR-0007 §4.3; the container showed
  why (a 2 ms wait returned after 5.9 ms).
- **Values: normalised everywhere.** LV2 has no normalised form, and the
  VST3 conversion is a controller call that cannot run on the processing
  thread.
- **Values: plain everywhere.** Every VST3 change would need a hop to the
  host's main thread for conversion, adding latency to every automation
  point.
- **Chain edits by splicing a running host.** Faster to apply, but a second
  path for structure next to crash restart, and a region that must grow
  while mapped. Revisit only if Unverified 5 shows edits too slow.
- **Bypass as a plan swap or an engine-side atomic.** A swap per bypass is
  heavy and not sample-placed; an engine-side bypass would need the dry
  delay line rejected above. A reserved parameter is block-tagged,
  automatable and compensated in the host.
- **Plugin state inline in the session JSON.** Megabytes in every Snapshot
  and undo window, against a 64 MiB wire cap.
- **Refuse a plan whose compensation exceeds the maximum.** Leaves the
  engine playing a plan the document no longer describes, with the reason
  invisible. Clamp and report in red instead (§7.3).
- **Depth 0 kept in v1.** Saves a block for monitoring, at the cost of drops
  whenever wake plus chain overruns the rest of a callback; no figure we
  have supports it.

## Implementation checklist, in order

1. **Contract acceptance** (`contract/plugin-v1`), rt-engineer and
   plugin-engineer, both reviews in the thread plus `code-reviewer`:
   `protocol.h` to layout 1 per §5 (tagged `ParamChange`, last-value table
   and the engine-only `values_seq`, `header_bytes`, `request_word` gone,
   `kBypassParam`, depth 1 only, exit code 3; rule text for the host's
   order: load `tick`, then `submitted_block`, then wait; table/ring
   precedence); the clock region header with `wake_mode`, sent as the
   read-only sealed descriptor; `host_control.h`; size and offset asserts;
   Unverified 2's test; `plugins/host` and its driver moved onto it, with
   the **timer-paced loop as the default** and the futex loop behind
   `wake_mode`; `CONTRACTS.lock`.
2. **Model and spec**, rt-engineer with ui-engineer review: §6 in
   `common/model` (types, operations, op codec, v1→v2 migration with a
   frozen v1 fixture, `Plugin State/` on the saver thread); then the
   additive `contracts/api` PR of §8 (protocol 3, state block layout 2,
   `PluginParameterChange`, `PluginCatalog`, `PluginFault`, the commands)
   with `FakeEngine` scripting plugins and a catalog and a conformance
   check for inserts. O1's default applies until the owner rules.
3. **Plan nodes and PDC** in `engine/core` and `plan_builder`, after #41
   merges: `TrackSource`, `InsertChain`, `TrackFader`; the §7.3 algorithm;
   clamp and report, and the `max_delayed_edges` cap; a test channel
   inside `engine/core`'s tests that
   behaves like a host with a declared latency, so the **2048-sample track
   lands aligned** (the DAW 5 exit ctest) before any host is involved.
4. **`engine/plugin_client`**, rt-engineer, auditor on every diff:
   `HostChannel` (collect, submit, tags, store by index with the
   per-callback copy cap and atomic stores, miss streak with fade-out,
   culprit, removal), the channel-slot table with epoch retire and
   generations (a regression test: a retired plan naming a reassigned slot
   collects nothing), `wake_all()` in `common/platform` behind
   `kHostWakeEnabled = false`, the host supervisor on the control thread
   (fork, pidfd, heartbeat, restart policy, make-before-break, region
   pre-touch, retire by epoch); end-to-end tests with the `crash`, `sleep`,
   `nan` and `liar` test plugins on the timer path; RTSan job over it;
   **the RTSan test for `wake_all()` (Unverified 10)**; **the reachability
   test that no other `common/platform` symbol is reachable from
   `Session::process`**; the start-up clocksource check.
5. **Control-server wiring**, rt-engineer: the scan worker and catalog
   cache, §6.3 operations driving host lifecycle and replans,
   `PluginParameterChange` to `RtCommand` by the instance's key table,
   `FlushPluginState` (`GetState`, `SetInsertState`, `SetInsertParams`,
   reply last), editor values recorded at gesture end and on
   `EditorClosed`, hosts started and stopped from the document diff of
   active insert ids, `PluginFault` events and the state
   block's insert fields, the editor commands, the start-up version check.
6. **UI insert slots**, ui-engineer: A–J slots on each mixer strip, the
   plugin picker from `PluginCatalog`, active/inactive and bypass, the
   chain latency and the session's compensation (red when clamped), fault
   states with retry/inactive/remove, the generic parameter panel from the
   descriptor; screenshots in the PR. O2 before third-party editors ship.
7. **Bare-metal wake measurement**, researcher, when the hardware is
   attached: Unverified 1 exactly as specified (18 and 64 waiting hosts,
   `stress-ng --futex` for at least 1 h, quantum 128, p99.9 and max with
   every host waiting, pass at max under about 130 µs), plus Unverified 9.
   Pass: an amendment sets `kHostWakeEnabled = true`, auditor reviewing.
   Fail: an amendment makes §1.3 the decision.
8. **Other bare-metal follow-up**: Unverified 3 and 4, then amendments for
   depth 0 and the chains-per-host question if the figures call for them.
