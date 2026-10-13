# ADR-0012 — Plugin hosting in the engine (M9): wake, granularity, misses, inserts and PDC

- Status: **Accepted in part**, 2026-10-13.
  - **Accepted (architect)**, as engineering choices inside ADR-0007 §4.3 and
    ADR-0011 §1.5–1.6: §2 (one host per insert chain), §3 (a missed block
    is silence), §4 (`HostControl`), §5 (the five contract items), §6
    (inserts in the model and the session format), §7 (plan nodes and PDC),
    §8 (the control API).
  - **Proposed, awaiting the rt-safety-auditor's ruling**: §1, the one wake
    syscall per callback, and the two other RT items listed under
    "For the rt-safety-auditor". The contract can be accepted before the
    ruling, because its layout does not depend on it (§1.4).
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

- The real-time path cannot allocate, lock or block. A wake syscall does
  not block, but it is still a syscall, and whether one is allowed in the
  callback is the auditor's to rule (ADR-0007 §4.3).
- Losing a take is the worst outcome. Capture is pre-insert
  (`plugin-latency-compensation.md`), so **no decision here can touch what
  lands on disk.** That is checked again in §7.
- The image boots the same way on hardware we have never seen. Plugin
  hosting must fail loudly at start-up when the host binary does not match
  the engine (§5.5), and must never stop the engine from recording.

## Decision

### 1. How the audio thread wakes the hosts

**1.1 One shared futex wake per callback, broadcast.**

- The engine creates one small **clock region** per session (a sealed memfd
  in `common/platform`, like the state block). It holds:
  - `tick`, a `uint32_t` word the audio thread increments once per callback
    in which it submitted to at least one host;
  - `callback_ns`, the `CLOCK_MONOTONIC` time the callback started (vDSO,
    no syscall), and `period_ns`. These serve the fallback in §1.3 and the
    host's own diagnostics.
- Every host maps it **read-only** and sleeps on `tick` with a shared
  `FUTEX_WAIT` and a timeout (100 ms, which also paces its heartbeat).
  Because the mapping is read-only, a scribbling plugin in one host cannot
  forge a wake for the others.
- The audio thread, **after the last `InsertChain` node of the plan has
  submitted** (§7.2), does `tick.fetch_add(1, release)` and **one**
  `FUTEX_WAKE` with `INT_MAX` waiters on it. Not one wake per host: one
  syscall per callback whatever the host count. The result is ignored. It
  never calls `FUTEX_WAIT`, never retries, never spins.
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

**1.3 If the auditor refuses a syscall in the callback.** The fallback
needs no change to the contract: hosts become **timer-paced**. Each host
sleeps with an absolute `clock_nanosleep` until `callback_ns + period_ns +
offset` (offset 150 µs, placeholder) and then checks `submitted_block`; if
the callback was late, it sleeps 50 µs and checks again, at most until
half a period has gone. The cost, stated: hosts wake on a guess, every
host takes a timer wake every period even when idle, and the time left to
process shrinks by the offset plus the timer's jitter (rtload woke
53 µs p50, 252 µs p99 late in the container). Expect more dropped blocks.
That is why it is the fallback, not the choice.

**1.4 Why the contract does not wait for the ruling.** The clock region,
the per-host header and every ordering rule are the same under §1.1 and
§1.3. Only whether `wake_all()` is called differs. So the contract (§5) is
accepted now, and `engine/plugin_client` calls `wake_all()` behind one
`constexpr bool` that the ruling sets.

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
  in linearly** from 0, so recovery is not a step. The onset of the hole
  cannot be ramped: that audio was never returned.
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
  as fd 3. The region memfd and the clock memfd cross on it with
  `SCM_RIGHTS`. Messages are framed and strictly decoded by the same rules
  as `contracts/api/wire.h` (type byte, little-endian fields, capped
  strings, refuse truncation and trailing bytes). The message set lives in
  `contracts/plugin/include/sonora/plugin/host_control.h`:

| Direction | Message | Purpose |
|---|---|---|
| engine → host | `Load` | chain: per instance format, identity (LV2 URI or VST3 class id), state bytes, dense parameter count |
| host → engine | `Hello` | layout version, wire version, pid; sent before anything else |
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
  - The engine keeps the **authoritative** copy in private memory, keyed by
    the chain's stable id (§7.1), so it survives a host replacement. The
    shared table is a mirror for the host and is never read back.
  - The audio thread writes the table entry before pushing the ring entry.
    A push refused because the ring is full sets `values_dirty`; the host,
    seeing it, re-reads the whole table at the start of its next block and
    applies every value that differs. A full ring therefore delays a value
    by at most one block and never loses it.
  - The control thread seeds the table from the model before the host
    starts, so the host instantiates with the right values and reports the
    right latency (a look-ahead parameter changes it).
  - On the **first submit to a new channel** the audio thread copies its
    private store into the region (at most 32 KiB, bounded) and sets
    `values_dirty`. That covers every change made between seeding and the
    swap. This copy is an auditor item.

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
  (`kMaxChannels`, default 256). A slot's lifetime is its host's. A plan
  swap that keeps the host keeps the slot, its block counters, its miss
  streak and its in-flight block: nothing resets. The per-chain private
  parameter store is keyed by the chain's stable id, so it survives a host
  replacement too.
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

**5.6 The wake.** As §1: one shared `FUTEX_WAKE` per callback, pending the
auditor; the timer-paced fallback otherwise.

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
| `RemoveInsert{insert}` | yes | its `InsertMix` entry outlives it, like `TrackMix` after an undone `AddTrack`, so redo restores its values |
| `MoveInsert{insert, to_track, to_slot}` | yes | between slots or tracks; refused onto an occupied slot |
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
| `InsertChain` | collects the chain's previous output from its channel, submits this block's input (§7.2) | `channel_slot`, `latency_frames`, `channels`, `chain_id` |
| `TrackFader` | step 3: the track's gain, mute, solo and pan from the `Track` atomics, ramped per block, into stereo | `track` |

- A track with inserts is `TrackSource → InsertChain → TrackFader →
  BusSum`. **Inserts are pre-fader**, as in Pro Tools and Logic, so a fader
  move never changes what a compressor sees.
- `PlanNode` gains `channel_slot` (the `HostChannel` slot, §5.4),
  `chain_id` (the chain's stable id: the track id, so the private
  parameter store follows the track), `latency_frames` (the chain's checked
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

1. If its channel's `completed_block` is the block submitted last callback
   (depth 1), copy the output planes into its scratch buffer (ramped in
   after a miss); otherwise silence, counted.
2. If the host is idle (`completed_block == submitted_block`), write its
   input from the upstream node, push any pending parameter entries,
   publish `submitted_block`; otherwise skip the submit and count it.
3. After the plan's last `InsertChain` node (the builder records its index
   as `last_insert_node`), one `wake_all()` if anything was submitted.

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
  `PluginParameterChange{seq, insert, param (dense index), value}`. The
  control thread maps it to the chain and sends it on as an `RtCommand`.
  `ParameterChange` stays track-only. The UI writes the end of a gesture
  with `SetInsertParams`, as faders will with `SetMix`.
- **The catalog:** a new engine → client message, `PluginCatalog`: per
  plugin, format, id, name, vendor, version, channels, usable or the
  refusal reason, has-editor, and per parameter its dense index, key, name,
  range, default and flags (automatable, toggle, enum, logarithmic, past
  the cap). Sent after the Snapshot and again after a rescan.
- **Commands** (catalog ids, `Engine` domain, key-mappable):
  `plugin.rescan`; `plugin.editor.open` and `plugin.editor.close` with the
  track as target and `"<slot> <parent token>"` as argument (the token
  is opaque and empty on the appliance); `plugin.retry` for a chain left
  down by §5.3.
- **Events:** `EventKind::PluginFault` (append-only): crashed, removed for
  misses, restarted, refused, non-finite output, missing at open, host
  incompatible. `track` and the text name the plugin and the slot.
- **State block, layout version 2:** per track an `InsertState` for each of
  the 10 slots: flags (present, active, bypassed, running, restarting,
  faulted, missing), latency frames, misses (monotonic), worst run in µs,
  non-finite samples (monotonic); in the header the total compensation and
  the over-maximum shortfall. If PR #41's planned layout-2 fields (locate,
  bus peaks) land first, this is layout 3.
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

## Rationale

- **What each choice costs later.**
  - *One broadcast wake* keeps the audio thread's syscall count at one,
    whatever the number of chains grows to. Per-host wakes would put 18 to
    64 syscalls in a callback at full mixes, and that number would only
    grow. If the auditor refuses even one, §1.3 is ready, and the contract
    does not move.
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
  the callback; the one syscall is non-blocking and gated. Capture is
  pre-insert in every case. Version mismatch is found when the engine
  starts, not when the operator inserts the first plugin.
- **Prior art for a wake from the RT thread (recalled, not verified in this
  repository).** JACK2 activates clients from its RT thread through futex
  or semaphore posts, and PipeWire's graph signals the next node from its
  data loop with an eventfd write. If that holds, our audio thread runs
  inside a server that already makes this kind of call every period. The
  researcher confirms it from the sources (Unverified 7); the auditor
  rules either way.

## Consequences

- **ADR-0007 §2's table changes (this is the amendment §6 requires for
  moving code into the RT boundary):** `common/platform` may contribute
  exactly `wake_all()` (`SONORA_RT`, auditor-gated) to the audio thread.
  Nothing else in `common/platform` may be reached from it.
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
- ADR-0009 §4.5's "refuse a plan over a maximum" has one exception
  (§7.3).
- The session format moves to version 2, and every file written by this
  build is unreadable by an older one, loudly (`session-format.md`,
  "Versioning").

## For the rt-safety-auditor

Rulings needed before `engine/plugin_client` merges. Each is code in the
callback's call graph.

- **A1. One `FUTEX_WAKE` (shared, `INT_MAX` waiters) per callback** on the
  clock region's `tick`, through `common/platform`'s `wake_all()`, after
  the last `InsertChain` node. Non-blocking by the futex API, but on
  `PREEMPT_RT` the futex hash-bucket lock is a sleeping lock with priority
  inheritance, so a contended wake can briefly block. The ruling should
  rest on Unverified 1. Refused means §1.3.
- **A2. The bounded copy** of a chain's private parameter store into a new
  region on its first submit: at most 32 KiB, once per channel lifetime.
- **A3. `common/platform` in the RT boundary** for `wake_all()` only.
- Already in the draft and still the auditor's: every host-written word
  checked before use; plane addresses from the private `Layout`; the masked
  parameter ring.

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

1. **The wake's cost in the callback, bare metal, RT kernel.** p50, p99.9
   and max of `wake_all()` measured inside the callback with 1, 18 and 64
   waiting hosts, against 18 per-host wakes; and dropped-block rate under
   18 chains. Settles A1 and depth 0. Researcher, with the
   "ADR-0007 IPC figures on the RT kernel" BACKLOG item.
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
6. **Silence with a ramp-in on a miss** is acceptable by ear compared with
   the alternatives. A listening check on forced misses (the `sleep` test
   plugin). Owner's ears, when a build plays.
7. **JACK2 and PipeWire wake syscalls from their RT threads** (Rationale).
   Researcher, from the sources.
8. **Default compensation maximum 16 384** is a placeholder that matches
   what Pro Tools operators expect; researcher to confirm the figure
   Avid documents for its delay compensation limit.

## Alternatives rejected

- **Wake: one wake per host.** Correct and simple, but *N* syscalls per
  callback, growing with the mix. Kept only as the comparison in
  Unverified 1.
- **Wake: eventfd.** Same cost per call as a futex (measured), but no clean
  broadcast, so it means one write per host, and one fd per host.
- **Wake: hosts spin.** Measured into RT throttling (46–51 ms stalls); a
  core per host. Making it work needs `sched_rt_runtime_us=-1`, which
  removes the kernel's protection against a runaway RT thread.
- **Wake: timer-paced hosts as the choice.** No syscall on the audio
  thread, but hosts wake on a guess and lose the timer jitter from every
  period. It is the fallback (§1.3).
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
   and `values_dirty`, `header_bytes`, `request_word` gone, `kBypassParam`,
   depth 1 only, exit code 3); the clock region header;
   `host_control.h`; size and offset asserts; Unverified 2's test;
   `plugins/host` and its driver moved onto it; `CONTRACTS.lock`.
   **In parallel:** the auditor's rulings A1–A3 (A1 provisional on
   Unverified 1; §1.4 keeps the work moving either way).
2. **Model and spec**, rt-engineer with ui-engineer review: §6 in
   `common/model` (types, operations, op codec, v1→v2 migration with a
   frozen v1 fixture, `Plugin State/` on the saver thread); then the
   additive `contracts/api` PR of §8 (protocol 3, state block layout 2,
   `PluginParameterChange`, `PluginCatalog`, `PluginFault`, the commands)
   with `FakeEngine` scripting plugins and a catalog and a conformance
   check for inserts. O1's default applies until the owner rules.
3. **Plan nodes and PDC** in `engine/core` and `plan_builder`, after #41
   merges: `TrackSource`, `InsertChain`, `TrackFader`; the §7.3 algorithm;
   clamp and report; a test channel inside `engine/core`'s tests that
   behaves like a host with a declared latency, so the **2048-sample track
   lands aligned** (the DAW 5 exit ctest) before any host is involved.
4. **`engine/plugin_client`**, rt-engineer, auditor on every diff:
   `HostChannel` (collect, submit, tags, store, miss streak, culprit,
   removal), the channel-slot table, `wake_all()` in `common/platform`,
   the host supervisor on the control thread (fork, pidfd, heartbeat,
   restart policy, make-before-break, retire by epoch); end-to-end tests
   with the `crash`, `sleep`, `nan` and `liar` test plugins; RTSan job over
   it.
5. **Control-server wiring**, rt-engineer: the scan worker and catalog
   cache, §6.3 operations driving host lifecycle and replans,
   `PluginParameterChange` to `RtCommand`, save (`GetState`,
   `SetInsertState`, `SetInsertParams`), `PluginFault` events and the state
   block's insert fields, the editor commands, the start-up version check.
6. **UI insert slots**, ui-engineer: A–J slots on each mixer strip, the
   plugin picker from `PluginCatalog`, active/inactive and bypass, the
   chain latency and the session's compensation (red when clamped), fault
   states with retry/inactive/remove, the generic parameter panel from the
   descriptor; screenshots in the PR. O2 before third-party editors ship.
7. **Bare-metal follow-up**, when the hardware is attached: Unverified 1,
   3 and 4, then an amendment for depth 0, the wake if needed, and the
   chains-per-host question.
