// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// ============================================================================
//  ACCEPTED CONTRACT, layout version 1 (ADR-0012 §5, checklist item 1).
//
//  Owned jointly by plugin-engineer and rt-engineer (ADR-0007 §4.3, §6). A
//  change to any field, ordering rule or who writes a word is a change to
//  this contract: kLayoutVersion (or kClockLayoutVersion) moves, and the
//  sizeof/offsetof asserts in tests/test_protocol.cpp move with it, or the
//  build fails. code-reviewer checks that they moved together.
//
//  The five items the draft left open are decided by ADR-0012 §5:
//    parameters      block-tagged, frame offset, last-value table (§5.1)
//    values          format-native: LV2 plain, VST3 normalised (§5.2)
//    crash/restart   a region is never reused (§5.3)
//    plan swaps      channel slots outside the plan, generation-checked (§5.4)
//    versioning      header_bytes, asserts, exit code 3 (§5.5)
//  and the rt-safety-auditor's rulings on PR #42 (A1, A2, H1, M1, M2, M3, L1)
//  are written into the rules below where they bind, as are the PR #45
//  acceptance reviews (rt-engineer M1-M4, auditor L1-L6).
//
//  The fd-3 socket messages of ADR-0012 §4 are host_control.h's, with their
//  own kHostWireVersion; it states how they are ordered with the region
//  rules below (the region is the truth, a message the wake-up). A host
//  started without --control-fd is told its chain on the command line.
// ============================================================================
//
// Two shared regions per host process, both created and sized by the engine:
//
// 1. The CHANNEL region, one per host, mapped read-write by both sides:
//
//      ChannelHeader                                    (this file)
//      last-value table [param_slots] atomic<float>     engine writes, host reads
//      input planes     [channels][max_frames] float    engine writes, host reads
//      output planes    [channels][max_frames] float    host writes, engine reads
//
// 2. The CLOCK region, one per session, shared by every host: a ClockHeader.
//    The engine writes it; every host maps it READ-ONLY, and that is enforced
//    by the descriptor, not left to the host (auditor M1): the engine hands
//    over an O_RDONLY reopen of a memfd sealed with F_SEAL_FUTURE_WRITE (the
//    path common/platform's SharedRegion already uses for the state block),
//    so a PROT_WRITE mapping of it fails with EACCES. A scribbling plugin in
//    one host cannot forge a tick or a callback time for the others.
//
// Platform-neutral on purpose (ADR-0008): fixed-width integers, std::atomic
// and offsets only. How a region is created and passed, and how a sleeping
// host is woken, live in a platform seam (common/platform/wake.h), never here.
//
// ---------------------------------------------------------------------------
// Trust. The host runs third-party code and maps the channel region
// writable, so a scribbling plugin can change ANY word of it, the "fixed"
// part and the engine -> host words included. Rules for the engine:
//
//  - Every word in the channel region is untrusted when the engine reads it:
//    completed_block, latency_frames, latency_generation, current_instance,
//    refusal, refusal_instance, state, host_flags, heartbeat, param_read,
//    params_rejected, params_late, the instance reports, the output planes.
//  - Such a word is bounds-checked before use (checked_instance,
//    checked_latency, checked_state below) and is NEVER turned into a
//    pointer, an offset or an index without that check.
//  - Plane and table addresses come from the engine's private Layout
//    (make_layout), computed from its own configuration, never from the
//    header's offsets, which exist for the host.
//  - The engine never reads back what it wrote: not the fixed part, not the
//    last-value table (its authoritative copy is private, §5.1), not
//    values_seq, param_write or submitted_block (EngineChannel below keeps
//    its own copy of each; auditor L1 on PR #45).
//  - The parameter ring index is masked, so a forged param_read can only
//    make the ring look full (pushes refused and counted), never move a
//    store outside the ring.
//  - The output planes are read once, into engine-private memory, and
//    checked there for NaN and Inf (collect_output, step 1 below). The
//    host's own scrub is not trusted: a plugin thread can write after it.
//  - The audio thread never reads latency_frames from a region: a plan
//    carries the figure the builder read, through checked_latency, on the
//    control thread (§5.4).
//
// The host, in turn, checks compatibility (layout_compatible), validates the
// header it was handed (header_valid) and builds its own Layout and ParamMap
// once from the validated values.
//
// ---------------------------------------------------------------------------
// Block protocol, pipeline depth 1 (the only depth v1 accepts, §1.5). The
// audio crosses once per block per host process (§4.3). The audio thread
// never waits on a host: no timed wait, no futex wait, no spin. It submits,
// and it collects only what is already there.
//
//  Block numbering (rt-engineer M1 on PR #45). Per channel: the first block
//  submitted to a channel is 1, and every submit is the previous one + 1,
//  with no gaps. A plan swap that keeps the node on the same channel keeps
//  the count; a new channel (a new region) starts again at 1. Block 0 is
//  never submitted: it is what submitted_block and completed_block hold in
//  a fresh region, so the host's "submitted_block != last" never sees work
//  that was not sent. The count lives in the engine's EngineChannel, never
//  in the region; submit_block assigns it, next_block is the tag for
//  changes sent before it.
//
//  Frame count (rt-engineer M3). In v1 every block is exactly block_frames
//  frames: the quantum is pinned (ADR-0007 M2), latency_frames declares
//  pipeline_depth * block_frames, and depth 1 plays the previous callback's
//  block, so a block of any other size would break PDC. A callback whose
//  frame count is not block_frames is a miss for every InsertChain node:
//  nothing collected, nothing submitted, every change held (step 2),
//  counted. submit_block refuses such a count. A host that reads
//  submitted_frames != block_frames processes nothing and faults
//  (kHostExitFaulted): only a scribbling plugin can have written it.
//
//  Engine, callback N (RT), per InsertChain node, in plan order:
//    0. The node's channel slot generation must equal the generation the
//       plan was built against (slot_current); otherwise this is a miss,
//       counted as a fault, and nothing below is read or written (H1).
//    1. If completed_block is the block submitted in callback N - 1, copy
//       the output planes out with collect_output: each plane is copied
//       once into engine-private memory and only that copy is checked and
//       used; NaN and Inf in it are replaced by 0 and counted. The engine
//       owns this guard (rt-engineer M4); the host's scrub is not trusted.
//       A non-zero count makes the block a miss, and a fault against the
//       node; the zeroed copy is never played. Otherwise that copy is this
//       callback's output. A block that completed later than callback N is
//       discarded, never played late, which would break the declared
//       latency. Not collected: a miss (ADR-0012 §3: fade the last sample
//       out over 64 frames, then silence, counted; ramp the next collected
//       block in).
//    2. If completed_block != submitted_block the host is still busy with an
//       older block: do NOT write the input planes, skip the submit, count
//       it. This rule is what makes one input and one output slot
//       sufficient: the engine never writes a slot the host may be reading,
//       and never reads one the host may be writing.
//       Otherwise, in this order: if the channel's resync flag is set
//       (its first submit, or a change held in an earlier callback),
//       store_values, under the per-callback copy budget (A2); write the
//       input planes; send this callback's changes for the node
//       (send_param, tagged next_block); then submit_block:
//       submitted_frames, then submitted_block.store(block, release).
//       Parameters in a callback that does NOT submit to the channel (a
//       busy host, a slot mismatch, a frame count that is not
//       block_frames; rt-engineer M2, auditor L5): NOTHING is written to
//       the region, not the table, not the ring, not values_seq. The
//       change goes into the engine's private store (authoritative anyway,
//       §5.1) and sets the channel's resync flag (hold_param). The host may
//       be reading the table for an OLDER block at that moment, which is
//       why the submit was skipped; a table write then could apply the
//       value a block early, at offset 0 of audio that came before the
//       change. Held, it reaches the host with the next submit, at offset
//       0: late by the skipped callbacks, never early, never lost. A
//       resync that does not fit this callback's copy budget stays set and
//       the submit goes ahead without it (late again, never early); only a
//       channel's FIRST submit waits for its copy (it plays silence either
//       way). With this rule the engine never writes the table while the
//       host is between a submit and its completion, so the table step
//       (c.2) can never pick up a later block's value.
//    3. After the plan's last InsertChain node: if anything was submitted,
//       publish_tick on the clock region; and only when kHostWakeEnabled,
//       one wake_all(tick) from common/platform. Skipped when nothing was
//       submitted, never retried, its result ignored (A1).
//
//  Host, its own thread, every pass, in this order (auditor M2):
//    a. Load the clock's tick (acquire), THEN load submitted_block
//       (acquire). A submit between the two has already changed tick, so a
//       futex wait on the value read in (a) returns at once instead of
//       sleeping through a block. Reversed, a lost wake costs the 100 ms
//       timeout, about 37 missed blocks, and the chain is removed at 8.
//    b. If submitted_block differs from the last block processed, process
//       it (c to e), with block_frames frames: a submitted_frames other
//       than block_frames is a fault (see Frame count). Otherwise sleep (clock.wake_mode): Timer, the default,
//       sleeps to the next expected callback (next_wake_ns, never more
//       than a period ahead) and polls in
//       50 us steps while that callback is late, for at most half a period;
//       Futex sleeps in FUTEX_WAIT(tick, value read in a) with a 100 ms
//       timeout. Bump heartbeat on every pass, idle or not.
//    c. Parameters for block N, in this order (auditor L1):
//         1. drain the ring: entries for blocks < N apply at offset 0 and
//            are counted in params_late; entries for N apply at their
//            offset; the first entry for a block > N stops the drain and
//            stays in the ring. Never more than kParamRingCapacity entries.
//         2. if values_seq differs from the last one the host saw (load it
//            with acquire, BEFORE reading the table), re-read the whole
//            table and apply every value that differs from what the host
//            last applied, at offset 0, replacing any point already queued
//            for that parameter in this block. The table entry is written
//            before its ring entry, so the table is never older than any
//            ring entry already pushed: the newest value wins.
//       consume_block_params below is that algorithm; the host uses it.
//    d. Run the chain, write the output planes.
//    e. completed_block.store(N, release); done_word.fetch_add(1, release);
//       wake done_word. Only the control thread (waiting for Ready) and
//       measurement harnesses sleep on done_word, never the audio thread.
//       That wake is a FUTEX_WAKE syscall per block per host, off the audio
//       thread but on PREEMPT_RT a hash-bucket lock all the same, so it is
//       inside ADR-0012's Unverified 1 measurement (auditor L4).
//       heartbeat sits on its own cache line, away from completed_block,
//       which the audio thread reads every callback (auditor L4).
//
// Parameters (ADR-0012 §5.1, §5.2):
//  - ParamChange::param is a DENSE index into the instance's parameter list
//    as the scan described it (`sonora-plugin-host --probe` and `--list`
//    print it): the writable parameters in the plugin's own order (LV2:
//    input control ports by port index; VST3: writable parameters in
//    controller index order). Never a VST3 ParamID or an LV2 port index on
//    the wire. instance_params[] in the fixed part says how many each
//    instance has as the engine's scan saw it, and a host that counts
//    differently refuses the chain (Refusal::ParamMismatch).
//  - ParamChange::value is format-native: the LV2 port value (plain, inside
//    the port's range) for an LV2 instance, the VST3 NORMALISED value (0..1)
//    for a VST3 instance. The engine only clamps, to the descriptor's range
//    or to [0, 1]; nothing on any RT path converts.
//  - kBypassParam as `param` is the instance's bypass: value > 0.5 bypasses.
//    Block-tagged and automatable like any parameter; the host implements it
//    latency-compensated (the plugin's designated bypass parameter, else a
//    host-side delay of the instance's latency), so a toggle never shifts
//    timing. Its last value lives in bypass[], under the same values_seq.
//  - The audio thread is the ring's ONLY producer (SPSC). UI changes, editor
//    touches and automation reach it as RtCommands and it tags them.
//  - The last-value table holds param_slots values: the chain's dense
//    parameters, instance after instance, capped at kMaxParamsPerChain.
//    Parameters past the cap have no slot and are not automatable from
//    Sonora (a push for one is rejected by the host and counted).
//  - values_seq is written by the ENGINE ONLY (auditor M3). The host keeps
//    the last value it saw in its own memory; no host-written word is on
//    this path, so a set landing while the host reads the table bumps the
//    counter again and is picked up next block. A full ring therefore moves
//    a value by at most one block and never loses it.
//
// Regions and slots are never reused (ADR-0012 §5.3, §5.4, auditor H1):
//  - A region a dead or killed host touched is unmapped and closed, never
//    handed to another host. It is released only after every plan that
//    referenced its channel has been retired (ADR-0009 §4.1's epoch rule).
//  - The engine's channel slots (engine/plugin_client, kMaxChannels) carry a
//    SlotGeneration, bumped by the control thread when it assigns the slot
//    to a new host and before publishing a plan that uses it; a slot returns
//    to the free list only after every plan naming it is retired. The plan
//    node carries the generation it was built against and the audio thread
//    checks it in collect and submit (slot_current); a mismatch is a miss
//    and a fault, never data.
//  - Two guards, in this order. The FIRST is the epoch rule: a slot is
//    reassigned only after every plan naming it has been retired, so no
//    running plan can name it. The generation is the SECOND, for the
//    window between a plan's check and its use (TOCTOU) and for a retire
//    bug. Ordering: the control thread writes the slot's whole payload
//    (region base, Layout, ParamMap, store index, a fresh EngineChannel
//    with resync set, the node's RT counters reset) BEFORE assign_slot's
//    release store; the audio thread's slot_current is an acquire load, so
//    a node that sees the new generation sees that payload.
//  - Restart and chain edits are make-before-break: a NEW region and a new
//    host are started and brought to Ready while the old host keeps
//    playing; a plan swap moves the node to the new slot; the old host is
//    shut down, and its region released, after the old plan is retired.
//    One code path for load, edit and crash. The first submit to the new
//    channel copies the engine's private parameter store into its table
//    (store_values) before submitting.
//  - A swap that changes a node's channel discards the old channel's
//    in-flight block. A latency change (latency_generation) keeps the old
//    alignment until the control thread has re-planned.
//
// Non-finite output. The host replaces every NaN or Inf a plugin writes with
// 0 before the next plugin, or the engine, sees it, and adds the count to
// that plugin's InstanceReport::nonfinite_samples. A plugin that produces
// NaN or Inf from silence is refused at validation (NonFiniteOutput).
// That scrub is the host's courtesy, not the engine's guarantee: the output
// planes are untrusted, so the engine checks its own copy (collect_output,
// block protocol step 1) and never lets a non-finite sample into the mix.
//
// Denormals. The host sets flush-to-zero and denormals-are-zero on its
// processing thread before the first block; kHostFlushDenormals says it did.
//
// Latency: latency_frames is the whole chain's latency PLUS
// pipeline_depth * block_frames (§4.3: IPC delay is declared like any
// look-ahead, one mechanism for PDC). It may change while running;
// latency_generation is bumped after every change, and the control thread
// re-plans. A value above kMaxLatencyFrames is a fault, not a plan; the
// host clamps a plugin's own figure to it before sizing anything by it.
// Publish order: the host writes latency_frames (relaxed), then bumps
// latency_generation (release), and only after that stores state = Ready
// (release). So the control thread loads state with acquire, and on Ready
// loads latency_generation (acquire) and then latency_frames, through
// checked_latency; it never reads a latency before it has seen Ready.
//
// The channel's sample_rate and block_frames must equal the clock's
// (channel_matches_clock): the host paces on the clock's period and
// declares latency from the channel's block_frames. A sample_rate of 0 is
// not valid in either header.
//
// Versioning (ADR-0012 §5.5). The first three words of both headers (magic,
// layout_version, header_bytes) never move in any version. A host that does
// not speak the engine's layout or clock version prints one stderr line
// naming both and exits with kHostExitIncompatible (3) BEFORE writing
// anything to either region. The engine runs `sonora-plugin-host --version`
// at start-up, and a mismatch disables plugin hosting for the run, loudly.
// The line's grammar is fixed (format_version_line, parse_version_line):
//
//     sonora-plugin-host layout <N> clock <M>
//
// the program name, then `key value` pairs, one space apart, values in
// decimal, on one stdout line. More pairs may follow in later versions; a
// parser skips keys it does not know and refuses a line without both layout
// and clock. The host prints host_control.h's line, which appends
// `wire <kHostWireVersion>`, and the engine parses it with
// parse_host_version_line, which requires all three.
// ---------------------------------------------------------------------------

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#include "sonora/rt.h"

namespace sonora::plugin {

inline constexpr uint32_t kChannelMagic = 0x48504E53u;  // "SNPH", little-endian
inline constexpr uint32_t kLayoutVersion = 1;
inline constexpr uint32_t kClockMagic = 0x43504E53u;    // "SNPC"
inline constexpr uint32_t kClockLayoutVersion = 1;

inline constexpr uint32_t kMaxInstances = 16;        // per host process (ADR-0012 §2: 10 used)
inline constexpr uint32_t kParamRingCapacity = 256;  // power of two
inline constexpr uint32_t kMaxParamsPerChain = 8192;
inline constexpr std::size_t kCacheLine = 64;
// v1 accepts depth 1 only (ADR-0012 §1.5). The field stays in the header so
// that re-admitting depth 0 by amendment is not a layout change.
inline constexpr uint32_t kPipelineDepth = 1;
// Above this a reported latency is corrupt: 2^20 frames is 21.8 s at 48 kHz,
// far past any look-ahead a mixing plugin needs.
inline constexpr uint32_t kMaxLatencyFrames = 1u << 20;
inline constexpr uint32_t kLatencyInvalid = 0xFFFFFFFFu;
// ParamChange::param for an instance's bypass (ADR-0012 §5.1).
inline constexpr uint32_t kBypassParam = 0xFFFFFFFFu;

// The audio thread's FUTEX_WAKE per callback (ADR-0012 §1.1, auditor A1):
// OFF until the bare-metal measurement of Unverified 1 passes and an
// amendment turns it on. Hosts run timer-paced (WakeMode::Timer) meanwhile.
// engine/plugin_client calls wake_all() only behind this constant.
inline constexpr bool kHostWakeEnabled = false;

// A2: the most the audio thread copies into tables in one callback when
// channels get their first submit (placeholder, about 16 384 values). A
// channel whose copy would pass it waits a callback; it plays silence either
// way. One chain's whole table always fits, so no channel waits for ever.
inline constexpr std::size_t kValueCopyBudgetBytes = 64 * 1024;

// Exit codes of `sonora-plugin-host --shm-fd` (and --probe). 3 is reserved
// for "this host does not speak the engine's versions" (ADR-0012 §5.5), so
// the engine can report an install fault naming the binary.
inline constexpr int kHostExitClean = 0;
inline constexpr int kHostExitUsage = 2;         // usage, or a channel that is not valid
inline constexpr int kHostExitIncompatible = 3;  // layout or clock version mismatch
inline constexpr int kHostExitFaulted = 4;       // a plugin misbehaved while running
inline constexpr int kHostExitRefused = 5;       // the chain was refused at load or validation

enum class HostState : uint32_t {
    Starting = 0,  // mapped, loading and validating the chain
    Ready = 1,     // validated; latency_frames is meaningful; accepts blocks
    Refused = 2,   // a plugin failed validation; see refusal_*; host exits
    Faulted = 3,   // a plugin misbehaved while running; host exits
    Stopped = 4,   // clean exit after shutdown was requested
};

// Why a chain was refused or faulted. A count, not a message: the message
// goes to the host's stderr and, later, over the HostControl socket.
enum class Refusal : uint32_t {
    None = 0,
    NotFound = 1,             // no plugin with that URI (LV2) or class id (VST3)
    ChannelMismatch = 2,      // declared audio ports or buses cannot map to `channels`
    UnsupportedFeature = 3,   // requires an LV2 feature the host lacks; VST3: no
                              // IAudioProcessor or no 32-bit float processing
    UnsupportedPort = 4,      // a port type the host cannot connect (CV, ...)
    InstantiateFailed = 5,
    WroteOutsideBuffers = 6,  // declared N channels, wrote more (canary)
    NonFiniteOutput = 7,      // NaN or Inf from silence during validation
    TooManyInstances = 8,
    ParamMismatch = 9,        // dense parameter count differs from instance_params
};

// How hosts sleep between blocks (ADR-0012 §1.3, §1.4). Written by the
// engine's control thread into the clock header at creation.
enum class WakeMode : uint32_t {
    Timer = 0,  // absolute timer to the next expected callback; the default
    Futex = 1,  // FUTEX_WAIT on tick; only once kHostWakeEnabled is true
};

// Engine -> host parameter change, 24 bytes. See "Parameters" above.
struct ParamChange {
    uint64_t block;     // the submitted_block it belongs to
    uint32_t instance;  // chain index
    uint32_t param;     // dense index, or kBypassParam
    uint32_t offset;    // frame inside the block, < submitted_frames
    float value;        // format-native
};

// Host -> engine, one per chain instance. Written by the host only, so
// untrusted (see Trust).
struct InstanceReport {
    std::atomic<uint32_t> latency_frames;  // this plugin's own, no IPC part
    std::atomic<uint32_t> replicas;        // mono plugin run once per channel
    std::atomic<uint64_t> last_run_ns;
    std::atomic<uint64_t> worst_run_ns;       // worst case, never an average
    std::atomic<uint64_t> nonfinite_samples;  // NaN/Inf it wrote, replaced by 0
};

struct ChannelHeader {
    // --- fixed: written by the engine before the host starts. The first
    // three words never move in any layout version. The host can still
    // overwrite all of it (see Trust); the engine never reads it back.
    uint32_t magic;
    uint32_t layout_version;
    uint32_t header_bytes;     // sizeof(ChannelHeader) of layout_version
    uint32_t sample_rate;
    uint32_t max_frames;       // per block, the size of every plane
    uint32_t block_frames;     // nominal quantum; used for the IPC latency
    uint32_t channels;         // audio channels in and out of the chain
    uint32_t pipeline_depth;   // kPipelineDepth
    uint32_t instance_count;   // plugins in the chain, <= kMaxInstances
    int32_t host_rt_priority;  // SCHED_FIFO priority to ask for; 0 = none
    uint32_t param_slots;      // entries in the last-value table
    uint32_t reserved0;
    uint64_t values_offset;    // byte offsets from the start of the region
    uint64_t input_offset;
    uint64_t output_offset;
    uint64_t region_size;
    // Dense parameter count of each instance, as the engine's scan saw it.
    uint32_t instance_params[kMaxInstances];

    // --- engine -> host. Only the engine writes these.
    alignas(kCacheLine) std::atomic<uint64_t> submitted_block;
    std::atomic<uint32_t> submitted_frames;
    std::atomic<uint32_t> shutdown;    // non-zero: finish and exit
    std::atomic<uint32_t> values_seq;  // bumped (release) after table writes the ring did not carry
    uint32_t reserved1;
    std::atomic<float> bypass[kMaxInstances];  // last-value table for kBypassParam

    // --- parameter ring, SPSC: the audio thread produces, the host consumes.
    // Indices run free and are masked; write - read is the fill.
    alignas(kCacheLine) std::atomic<uint32_t> param_write;
    alignas(kCacheLine) std::atomic<uint32_t> param_read;
    alignas(kCacheLine) ParamChange params[kParamRingCapacity];

    // --- host -> engine. Untrusted, every word (see Trust). The audio
    // thread reads completed_block every callback, so its line holds only
    // words the host writes once per block or less (auditor L4).
    alignas(kCacheLine) std::atomic<uint64_t> completed_block;
    std::atomic<uint32_t> done_word;     // control thread only, never the audio thread
    std::atomic<uint32_t> state;         // HostState
    // --- host -> engine, read by the control thread only. heartbeat moves
    // every 50 us while the host polls a late callback; current_instance
    // moves per plugin per block.
    alignas(kCacheLine) std::atomic<uint64_t> heartbeat;  // bumped on every pass, at least every 100 ms
    std::atomic<uint32_t> latency_frames;      // chain + pipeline_depth * block_frames
    std::atomic<uint32_t> latency_generation;  // bumped after latency_frames changes
    std::atomic<int32_t> current_instance;     // chain index being run, -1 between
    std::atomic<uint32_t> refusal;             // Refusal
    std::atomic<int32_t> refusal_instance;     // which chain index, -1 if none
    std::atomic<uint32_t> host_flags;          // HostFlag bits
    std::atomic<uint64_t> worst_block_ns;      // host-side, whole chain
    // Ring entries the host could not apply: no such instance or parameter
    // slot, or a non-finite value. Counted, never silently dropped.
    std::atomic<uint64_t> params_rejected;
    // Ring entries for a block older than the one processed, applied at
    // offset 0 (the host was late, or the engine tagged for a skipped block).
    std::atomic<uint64_t> params_late;

    InstanceReport instances[kMaxInstances];
};

// The session's clock (ADR-0012 §1.1). Written by the engine only; every
// host maps it read-only (M1).
struct ClockHeader {
    // --- fixed, written by the control thread at creation. The first three
    // words never move in any version.
    uint32_t magic;
    uint32_t layout_version;
    uint32_t header_bytes;  // sizeof(ClockHeader) of layout_version
    uint32_t wake_mode;     // WakeMode
    uint32_t sample_rate;
    uint32_t block_frames;
    uint64_t period_ns;     // block_frames at sample_rate; the quantum is pinned (ADR-0007 M2)

    // --- the audio thread.
    alignas(kCacheLine) std::atomic<uint32_t> tick;  // +1 per callback that submitted
    uint32_t reserved0;
    // CLOCK_MONOTONIC at the start of the server's latest period, as the
    // backend stamped it (contracts/backend AudioBlock::callback_ns; JACK's
    // cycle time), written before any submit in that callback. The callback
    // reads no clock for it (L2). Never 0 once written: an unstamped period
    // leaves the last stamp (ADR-0012 §1.1).
    std::atomic<uint64_t> callback_ns;
};

enum HostFlag : uint32_t {
    kHostRealtime = 1u << 0,        // got SCHED_FIFO at host_rt_priority
    kHostMemoryLocked = 1u << 1,    // mlockall succeeded
    kHostFlushDenormals = 1u << 2,  // FTZ and DAZ set on the processing thread
};

// Shared memory crosses processes, so every atomic in it must be address-free,
// which for GCC and Clang means always lock-free (ADR-0007 update 1, item 2:
// nothing wider than 8 bytes). The wake seam treats the 32-bit words as
// plain integers, so their representation must be exactly that.
static_assert(std::atomic<uint32_t>::is_always_lock_free, "u32 atomics must be lock-free");
static_assert(std::atomic<int32_t>::is_always_lock_free, "i32 atomics must be lock-free");
static_assert(std::atomic<uint64_t>::is_always_lock_free, "u64 atomics must be lock-free");
static_assert(std::atomic<float>::is_always_lock_free, "float atomics must be lock-free");
static_assert(sizeof(std::atomic<float>) == sizeof(float), "table entries are plain floats");
static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t), "wake words are plain u32");
static_assert(alignof(std::atomic<uint32_t>) == alignof(uint32_t), "wake words are plain u32");
static_assert(sizeof(ParamChange) == 24, "ParamChange is 24 bytes (ADR-0012 §5.1)");
static_assert(kMaxParamsPerChain * sizeof(float) <= kValueCopyBudgetBytes,
              "one chain's whole table must fit in one callback's copy budget");
static_assert((kParamRingCapacity & (kParamRingCapacity - 1)) == 0, "ring capacity is a power of two");

constexpr std::size_t align_up(std::size_t n, std::size_t a) { return (n + a - 1) / a * a; }

constexpr std::size_t plane_bytes(uint32_t channels, uint32_t max_frames) {
    return align_up(std::size_t(channels) * max_frames * sizeof(float), kCacheLine);
}

constexpr std::size_t table_bytes(uint32_t param_slots) {
    return align_up(std::size_t(param_slots) * sizeof(float), kCacheLine);
}

constexpr std::size_t values_offset() { return align_up(sizeof(ChannelHeader), kCacheLine); }

constexpr std::size_t input_offset(uint32_t param_slots) {
    return values_offset() + table_bytes(param_slots);
}

constexpr std::size_t output_offset(uint32_t channels, uint32_t max_frames, uint32_t param_slots) {
    return input_offset(param_slots) + plane_bytes(channels, max_frames);
}

constexpr std::size_t region_size(uint32_t channels, uint32_t max_frames, uint32_t param_slots) {
    return output_offset(channels, max_frames, param_slots) + plane_bytes(channels, max_frames);
}

// Where the table and the planes are. Each side keeps its own copy in
// private memory, computed from values it trusts: the engine from its
// configuration, the host from a header that passed header_valid. Never
// re-read from the region, which the other side, or a plugin, can overwrite.
struct Layout {
    uint32_t channels = 0;
    uint32_t max_frames = 0;
    uint32_t param_slots = 0;
    std::size_t values_offset = 0;
    std::size_t input_offset = 0;
    std::size_t output_offset = 0;
    std::size_t region_size = 0;
};

constexpr Layout make_layout(uint32_t channels, uint32_t max_frames, uint32_t param_slots) {
    return Layout{channels,
                  max_frames,
                  param_slots,
                  values_offset(),
                  input_offset(param_slots),
                  output_offset(channels, max_frames, param_slots),
                  region_size(channels, max_frames, param_slots)};
}

// Where each instance's parameters sit in the table. Built by each side from
// the per-instance dense counts it trusts (the engine from its scan, the
// host from a validated header), once, in private memory.
struct ParamMap {
    uint32_t instance_count = 0;
    uint32_t total = 0;        // every dense parameter of the chain
    uint32_t param_slots = 0;  // min(total, kMaxParamsPerChain): the table
    uint32_t base[kMaxInstances] = {};
    uint32_t count[kMaxInstances] = {};
};

inline ParamMap make_param_map(const uint32_t* counts, uint32_t instance_count) {
    ParamMap m;
    m.instance_count = instance_count < kMaxInstances ? instance_count : kMaxInstances;
    uint64_t total = 0;
    for (uint32_t i = 0; i < m.instance_count; ++i) {
        m.base[i] = total > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(total);
        m.count[i] = counts[i];
        total += counts[i];
    }
    m.total = total > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(total);
    m.param_slots = m.total < kMaxParamsPerChain ? m.total : kMaxParamsPerChain;
    return m;
}

// The table slot of (instance, dense param), or -1 when there is none: no
// such instance or parameter, or past kMaxParamsPerChain. RT-safe.
inline int64_t param_slot(const ParamMap& m, uint32_t instance, uint32_t param) noexcept {
    if (instance >= m.instance_count || param >= m.count[instance]) return -1;
    const uint64_t slot = uint64_t(m.base[instance]) + param;
    return slot < m.param_slots ? int64_t(slot) : -1;
}

// Channel c of a plane; `region` is the mapped base address. With
// c < layout.channels, an RT memcpy of up to max_frames samples stays inside
// the region whatever the shared header says.
inline float* input_plane(void* region, const Layout& l, uint32_t c) noexcept {
    return reinterpret_cast<float*>(static_cast<unsigned char*>(region) + l.input_offset) +
           std::size_t(c) * l.max_frames;
}
inline float* output_plane(void* region, const Layout& l, uint32_t c) noexcept {
    return reinterpret_cast<float*>(static_cast<unsigned char*>(region) + l.output_offset) +
           std::size_t(c) * l.max_frames;
}
// The last-value table: layout.param_slots entries.
inline std::atomic<float>* value_table(void* region, const Layout& l) noexcept {
    return reinterpret_cast<std::atomic<float>*>(static_cast<unsigned char*>(region) +
                                                 l.values_offset);
}

// Fault every page of a fresh region in before it is used, whatever the
// mlockall state (A2 condition 4): MCL_FUTURE covers a mapping only if the
// lock succeeded, and the audio thread's first-submit copy must not fault.
// Control thread, before init_header, on a zeroed region (writes zeros).
// Faulting in is not keeping in: if mlockall failed, shared pages can be
// reclaimed under memory pressure. So the control thread also locks each
// region it creates (mlock, or MAP_LOCKED at the mapping), and a failure is
// counted and reported with the plugin health flags, never silent (auditor
// L3; the driver's lock_region does it today, engine/plugin_client next).
inline void prefault(void* region, std::size_t bytes) noexcept {
    auto* p = static_cast<volatile unsigned char*>(region);
    for (std::size_t i = 0; i < bytes; i += 4096) p[i] = 0;
    if (bytes) p[bytes - 1] = 0;
}

// The engine side's half of initialisation. The region must be zeroed and at
// least layout.region_size bytes; `params` holds `instance_count` dense
// counts, and `map` built from them gives layout.param_slots. Not RT: done
// at creation. The engine keeps `layout` and `map`; the header's copy is for
// the host. The table starts at 0: the control thread seeds it (seed_value)
// from the model before starting the host, which applies it before
// validation, so the latency it reports is the one of those values.
inline ChannelHeader* init_header(void* region, const Layout& layout, uint32_t sample_rate,
                                  uint32_t block_frames, uint32_t instance_count,
                                  const uint32_t* params, int32_t host_rt_priority) {
    auto* h = new (region) ChannelHeader{};
    h->magic = kChannelMagic;
    h->layout_version = kLayoutVersion;
    h->header_bytes = static_cast<uint32_t>(sizeof(ChannelHeader));
    h->sample_rate = sample_rate;
    h->max_frames = layout.max_frames;
    h->block_frames = block_frames;
    h->channels = layout.channels;
    h->pipeline_depth = kPipelineDepth;
    h->instance_count = instance_count;
    h->host_rt_priority = host_rt_priority;
    h->param_slots = layout.param_slots;
    h->values_offset = layout.values_offset;
    h->input_offset = layout.input_offset;
    h->output_offset = layout.output_offset;
    h->region_size = layout.region_size;
    for (uint32_t i = 0; i < instance_count && i < kMaxInstances; ++i) h->instance_params[i] = params[i];
    h->current_instance.store(-1, std::memory_order_relaxed);
    h->refusal_instance.store(-1, std::memory_order_relaxed);
    return h;
}

// The words that never move. A host checks these first; a mismatch is "not
// my version" (exit kHostExitIncompatible, nothing written), anything wrong
// after that is "not a valid channel" (exit kHostExitUsage).
struct VersionWords {
    uint32_t magic, layout_version, header_bytes;
};

inline VersionWords version_words(const void* region) {
    VersionWords v;
    std::memcpy(&v, region, sizeof v);
    return v;
}

inline bool channel_compatible(const VersionWords& v) {
    return v.layout_version == kLayoutVersion && v.header_bytes == sizeof(ChannelHeader);
}

inline bool clock_compatible(const VersionWords& v) {
    return v.layout_version == kClockLayoutVersion && v.header_bytes == sizeof(ClockHeader);
}

// The checks a host makes before trusting a channel region it was handed,
// after channel_compatible.
inline bool header_valid(const ChannelHeader& h, std::size_t mapped_size) {
    if (h.magic != kChannelMagic || h.layout_version != kLayoutVersion ||
        h.header_bytes != sizeof(ChannelHeader))
        return false;
    if (h.sample_rate == 0 || h.channels == 0 || h.max_frames == 0 || h.block_frames == 0 ||
        h.block_frames > h.max_frames || h.pipeline_depth != kPipelineDepth ||
        h.instance_count == 0 || h.instance_count > kMaxInstances)
        return false;
    // Bounded before any product is formed, so nothing below can overflow.
    if (h.channels > 1024 || h.max_frames > (1u << 20)) return false;
    const ParamMap m = make_param_map(h.instance_params, h.instance_count);
    for (uint32_t i = h.instance_count; i < kMaxInstances; ++i)
        if (h.instance_params[i] != 0) return false;
    return h.param_slots == m.param_slots && h.values_offset == values_offset() &&
           h.input_offset == input_offset(h.param_slots) &&
           h.output_offset == output_offset(h.channels, h.max_frames, h.param_slots) &&
           h.region_size == region_size(h.channels, h.max_frames, h.param_slots) &&
           h.region_size <= mapped_size;
}

// The host's Layout and ParamMap, from a header that passed header_valid.
// Taken once; the host does not re-read the fixed part either.
inline Layout layout_of(const ChannelHeader& h) {
    return make_layout(h.channels, h.max_frames, h.param_slots);
}
inline ParamMap param_map_of(const ChannelHeader& h) {
    return make_param_map(h.instance_params, h.instance_count);
}

// ---- The clock region.

constexpr uint64_t period_ns_of(uint32_t block_frames, uint32_t sample_rate) {
    return sample_rate ? uint64_t(block_frames) * 1000000000ull / sample_rate : 0;
}

// Control thread, at session creation, on a zeroed region of at least
// sizeof(ClockHeader) bytes, before the descriptor is sealed and shared.
inline ClockHeader* init_clock(void* region, uint32_t sample_rate, uint32_t block_frames,
                               WakeMode mode) {
    auto* c = new (region) ClockHeader{};
    c->magic = kClockMagic;
    c->layout_version = kClockLayoutVersion;
    c->header_bytes = static_cast<uint32_t>(sizeof(ClockHeader));
    c->wake_mode = static_cast<uint32_t>(mode);
    c->sample_rate = sample_rate;
    c->block_frames = block_frames;
    c->period_ns = period_ns_of(block_frames, sample_rate);
    return c;
}

inline WakeMode default_wake_mode() { return kHostWakeEnabled ? WakeMode::Futex : WakeMode::Timer; }

inline bool clock_valid(const ClockHeader& c, std::size_t mapped_size) {
    return mapped_size >= sizeof(ClockHeader) && c.magic == kClockMagic &&
           c.layout_version == kClockLayoutVersion && c.header_bytes == sizeof(ClockHeader) &&
           c.wake_mode <= static_cast<uint32_t>(WakeMode::Futex) && c.sample_rate > 0 &&
           c.block_frames > 0 && c.period_ns == period_ns_of(c.block_frames, c.sample_rate) &&
           c.period_ns >= 1000 && c.period_ns <= 1000000000ull;
}

// The host's cross-check of its two headers, after header_valid and
// clock_valid: the clock paces it, the channel sizes its latency, so they
// must describe the same quantum.
inline bool channel_matches_clock(const ChannelHeader& h, const ClockHeader& c) {
    return h.sample_rate == c.sample_rate && h.block_frames == c.block_frames;
}

// ---- `sonora-plugin-host --version` (Versioning above). Not RT.

inline constexpr char kVersionProgram[] = "sonora-plugin-host";

// The host's line, without the newline, into `out`. Returns what
// snprintf returns.
inline int format_version_line(char* out, std::size_t size) {
    return std::snprintf(out, size, "%s layout %u clock %u", kVersionProgram, kLayoutVersion,
                         kClockLayoutVersion);
}

// The engine's parser. A trailing newline is allowed; keys it does not know
// are skipped with their value. False unless the program name comes first
// and both layout and clock are present, each once, in decimal that fits
// 32 bits.
inline bool parse_version_line(const char* line, uint32_t& layout, uint32_t& clock) {
    const std::size_t n = std::strlen(kVersionProgram);
    if (!line || std::strncmp(line, kVersionProgram, n) != 0) return false;
    const char* p = line + n;
    bool have_layout = false, have_clock = false;
    while (*p == ' ') {
        ++p;
        const char* key = p;
        while (*p >= 'a' && *p <= 'z') ++p;
        const std::size_t key_len = std::size_t(p - key);
        if (key_len == 0 || *p != ' ') return false;
        ++p;
        if (*p < '0' || *p > '9') return false;
        uint64_t v = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + uint64_t(*p - '0');
            if (v > 0xFFFFFFFFu) return false;
            ++p;
        }
        if (key_len == 6 && std::strncmp(key, "layout", 6) == 0) {
            if (have_layout) return false;
            have_layout = true;
            layout = uint32_t(v);
        } else if (key_len == 5 && std::strncmp(key, "clock", 5) == 0) {
            if (have_clock) return false;
            have_clock = true;
            clock = uint32_t(v);
        }
    }
    if (*p == '\n') ++p;
    return *p == '\0' && have_layout && have_clock;
}

// Audio thread, at the start of each callback, with the period's stamp.
// RT-safe. The engine does not call it with 0 (an unstamped period).
inline void publish_callback_start(ClockHeader& c, uint64_t now_ns) noexcept {
    c.callback_ns.store(now_ns, std::memory_order_release);
}

// Audio thread, after the plan's last InsertChain node, only when something
// was submitted (§7.2 step 3). The futex wake, when the gate is on, follows
// it through common/platform's wake_all(). RT-safe.
inline void publish_tick(ClockHeader& c) noexcept { c.tick.fetch_add(1, std::memory_order_release); }

// Host, timer mode: the next time worth looking for a submit, after `now`.
// A callback starts every period from callback_ns and submits within
// microseconds; `offset_ns` is how long after the start the host looks.
// Looking only at callback_ns + period (the draft) missed a block whenever
// the host went to sleep between a callback's start and its submit.
// Never more than one period after `now`: callback_ns is the server's
// estimate of its cycle's start, and jackd2's runs hundreds of ms ahead
// when its cycles run late (PR #77 RT audit B1). Followed as is, a stamp
// that far ahead would put the host to sleep through every block until
// then, and eight missed blocks remove the chain.
inline uint64_t next_wake_ns(uint64_t callback_ns, uint64_t period_ns, uint64_t offset_ns,
                             uint64_t now_ns) noexcept {
    if (period_ns == 0) return now_ns;
    uint64_t at = callback_ns + offset_ns;
    if (at > now_ns) return at - now_ns > period_ns ? now_ns + period_ns : at;
    const uint64_t behind = (now_ns - at) / period_ns + 1;
    return at + behind * period_ns;
}

// ---- Engine-side checks on host-written words. RT-safe.

// A chain index the host reported (current_instance, refusal_instance), or
// -1 when it is not an index into a chain of `count`.
inline int32_t checked_instance(int32_t reported, uint32_t count) noexcept {
    return reported >= 0 && static_cast<uint32_t>(reported) < count ? reported : -1;
}

// A latency the host reported, or kLatencyInvalid when it is implausible.
inline uint32_t checked_latency(uint32_t reported) noexcept {
    return reported <= kMaxLatencyFrames ? reported : kLatencyInvalid;
}

// The host's state word; a value outside the enum reads as Faulted.
inline HostState checked_state(uint32_t reported) noexcept {
    return reported <= static_cast<uint32_t>(HostState::Stopped) ? static_cast<HostState>(reported)
                                                                 : HostState::Faulted;
}

// ---- Channel slots (engine-private, never in a region; ADR-0012 §5.4, H1).
//
// One per slot of engine/plugin_client's fixed channel table. Generation 0
// means never assigned. The control thread calls assign_slot when it gives
// the slot to a new host, BEFORE publishing a plan that names the new
// generation, and only after every plan naming the previous host has been
// retired. The audio thread compares in collect and submit.
struct SlotGeneration {
    std::atomic<uint32_t> value{0};
};

inline uint32_t assign_slot(SlotGeneration& g) noexcept {
    uint32_t next = g.value.load(std::memory_order_relaxed) + 1;
    if (next == 0) next = 1;  // wrapped: 0 stays "never assigned"
    g.value.store(next, std::memory_order_release);
    return next;
}

inline bool slot_current(const SlotGeneration& g, uint32_t built_against) noexcept {
    return built_against != 0 && g.value.load(std::memory_order_acquire) == built_against;
}

// ---- One channel, engine side (engine-private, never in a region).
//
// What the engine keeps for each channel it writes, so that it never reads
// back a word it wrote (Trust, auditor L1): the block count (M1), the ring's
// write index, its count of values_seq, and whether the private store must
// be copied in before the next submit (first submit, or a held change, M2).
// Created by the control thread with the channel, as part of the slot's
// payload (SlotGeneration), and used by the audio thread only after that.
struct EngineChannel {
    uint64_t block = 0;         // the last block submitted; the first is 1
    uint32_t block_frames = 0;  // every block's frame count (M3)
    uint32_t param_write = 0;   // the ring's write index
    uint32_t values_seq = 0;    // the last values_seq stored
    bool resync = true;         // store_values before the next submit
};

inline EngineChannel make_engine_channel(uint32_t block_frames) noexcept {
    EngineChannel e;
    e.block_frames = block_frames;
    return e;
}

// ---- The block, engine side. RT-safe: no allocation, no wait.

// The tag for a change sent in this callback, before submit_block.
inline uint64_t next_block(const EngineChannel& e) noexcept { return e.block + 1; }

// Step 2's test: the host has completed every block submitted to it, so the
// engine may write the input planes, the table and the ring. completed_block
// is host-written and only ever compared with the engine's own count.
inline bool channel_idle(const ChannelHeader& h, const EngineChannel& e) noexcept {
    return h.completed_block.load(std::memory_order_acquire) == e.block;
}

// Step 2's last act. Refuses (writes nothing, returns false) a frame count
// other than the channel's block_frames (M3); the caller counts the miss.
inline bool submit_block(ChannelHeader& h, EngineChannel& e, uint32_t frames) noexcept {
    if (frames != e.block_frames) return false;
    h.submitted_frames.store(frames, std::memory_order_relaxed);
    h.submitted_block.store(++e.block, std::memory_order_release);
    return true;
}

// A change in a callback that does not submit to this channel (M2): the
// caller has written it to its private store; nothing goes to the region,
// and the next submit copies the store in.
inline void hold_param(EngineChannel& e) noexcept { e.resync = true; }

// Step 1's copy (rt-engineer M4). Copies `frames` samples of one output
// plane into `dst`, engine-private memory, then checks and repairs only
// that copy: every NaN or Inf in it becomes 0. Returns how many there were;
// non-zero makes the block a miss and a fault. Checking the copy, not the
// plane, is the point: a plugin thread can write the plane after any check
// of it. Bit tests, so -ffast-math cannot fold the check away.
inline uint32_t collect_output(float* dst, const float* plane, uint32_t frames) noexcept {
    std::memcpy(dst, plane, std::size_t(frames) * sizeof(float));
    uint32_t bad = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        uint32_t x;
        std::memcpy(&x, dst + i, sizeof x);
        if ((x & 0x7F800000u) == 0x7F800000u) {
            dst[i] = 0.f;
            ++bad;
        }
    }
    return bad;
}

// ---- Parameter ring and last-value table, engine side. RT-safe: no
// allocation, no wait. The audio thread is the only producer, and only in a
// callback that submits to the channel (block protocol step 2).

// A raw push. A full ring returns false. The write index is the engine's
// own (e.param_write); param_read is host-written, and the mask keeps the
// store inside the ring whatever it says: a value ahead of the write index
// reads as a full ring.
inline bool push_param(ChannelHeader& h, EngineChannel& e, const ParamChange& p) noexcept {
    const uint32_t w = e.param_write;
    const uint32_t r = h.param_read.load(std::memory_order_acquire);
    if (w - r >= kParamRingCapacity) return false;
    h.params[w & (kParamRingCapacity - 1)] = p;
    e.param_write = w + 1;
    h.param_write.store(e.param_write, std::memory_order_release);
    return true;
}

// The engine keeps its own count of values_seq; it never reads the shared
// word back.
inline void bump_values_seq(ChannelHeader& h, EngineChannel& e) noexcept {
    h.values_seq.store(++e.values_seq, std::memory_order_release);
}

// One change, the way the audio thread sends it (§5.1): the table entry
// first (relaxed), then the ring entry; a push the full ring refuses bumps
// values_seq so the host re-reads the table next block. Returns false when
// the ring refused it, for the caller to count; the value is not lost.
// A change for a parameter with no table slot is pushed anyway, so the host
// counts it as rejected. Only in a callback that submits to the channel,
// before submit_block, with p.block == next_block(e); otherwise hold_param.
inline bool send_param(ChannelHeader& h, EngineChannel& e, std::atomic<float>* table,
                       const ParamMap& m, const ParamChange& p) noexcept {
    if (p.param == kBypassParam) {
        if (p.instance < kMaxInstances) h.bypass[p.instance].store(p.value, std::memory_order_relaxed);
    } else if (const int64_t slot = param_slot(m, p.instance, p.param); slot >= 0) {
        table[slot].store(p.value, std::memory_order_relaxed);
    }
    if (push_param(h, e, p)) return true;
    bump_values_seq(h, e);
    return false;
}

// A resync (e.resync: a channel's first submit, or changes held while it
// did not submit): the engine's private store (param_slots values, and one
// bypass per instance) into the table, element by element with relaxed
// atomic stores, never memcpy (A2 conditions 1 and 3), then values_seq with
// release; clears e.resync. Only in a callback that submits, before
// submit_block. The caller charges value_copy_bytes against
// kValueCopyBudgetBytes for the callback first (A2 condition 2).
inline void store_values(ChannelHeader& h, EngineChannel& e, std::atomic<float>* table,
                         const Layout& l, const float* store, const float* bypass,
                         uint32_t instance_count) noexcept {
    for (uint32_t i = 0; i < l.param_slots; ++i) table[i].store(store[i], std::memory_order_relaxed);
    for (uint32_t i = 0; i < instance_count && i < kMaxInstances; ++i)
        h.bypass[i].store(bypass[i], std::memory_order_relaxed);
    bump_values_seq(h, e);
    e.resync = false;
}

constexpr std::size_t value_copy_bytes(const Layout& l) {
    return std::size_t(l.param_slots) * sizeof(float) + kMaxInstances * sizeof(float);
}

// Control thread, before the host starts: the value the host instantiates
// with. Not counted in values_seq; the host reads the whole table once at
// start-up.
inline void seed_value(std::atomic<float>* table, uint32_t slot, float value) noexcept {
    table[slot].store(value, std::memory_order_relaxed);
}

// ---- Parameter ring and last-value table, host side. Single consumer.

inline bool peek_param(const ChannelHeader& h, ParamChange& out) noexcept {
    const uint32_t r = h.param_read.load(std::memory_order_relaxed);
    const uint32_t w = h.param_write.load(std::memory_order_acquire);
    if (r == w) return false;
    out = h.params[r & (kParamRingCapacity - 1)];
    return true;
}

inline void consume_param(ChannelHeader& h) noexcept {
    h.param_read.store(h.param_read.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

inline bool pop_param(ChannelHeader& h, ParamChange& out) noexcept {
    if (!peek_param(h, out)) return false;
    consume_param(h);
    return true;
}

// What the host remembers between blocks, in its own memory. `applied`
// holds map.param_slots values and `applied_bypass` kMaxInstances: what was
// last applied to each slot, for the table comparison. Atomic because the
// host's control thread reads them too: after a lost touch it compares the
// plugin with what was applied, not with a table change still in flight.
// Stored with release after the plugin has the value.
struct HostValues {
    std::atomic<float>* applied = nullptr;
    std::atomic<float> applied_bypass[kMaxInstances] = {};
    uint32_t seen_seq = 0;
};

inline bool same_bits(float a, float b) noexcept {
    uint32_t x, y;
    std::memcpy(&x, &a, sizeof x);
    std::memcpy(&y, &b, sizeof y);
    return x == y;
}

inline bool finite_bits(float f) noexcept {
    uint32_t x;
    std::memcpy(&x, &f, sizeof x);
    return (x & 0x7F800000u) != 0x7F800000u;
}

// Host, at the start of block `block` of `frames` frames: steps c.1 and c.2
// of the block protocol, in that order (L1). Calls
//   deliver(instance, param, offset, value)  a ring change at its offset
//                                            (param may be kBypassParam)
//   replace(instance, param, value)          a table value: at offset 0,
//                                            superseding any point already
//                                            queued for it in this block
// either of which returns false when the plugin will not take it (counted
// in params_rejected). Returns the number of ring entries consumed. No
// allocation, no lock: the processing thread's call.
template <class Deliver, class Replace>
uint32_t consume_block_params(ChannelHeader& h, const std::atomic<float>* table, const ParamMap& m,
                              HostValues& v, uint64_t block, uint32_t frames, Deliver&& deliver,
                              Replace&& replace) noexcept SONORA_RT {
    uint32_t consumed = 0;
    uint64_t late = 0, rejected = 0;
    ParamChange p;
    while (consumed < kParamRingCapacity && peek_param(h, p)) {
        if (p.block > block) break;  // a later block's: left in the ring
        consume_param(h);
        ++consumed;
        const bool is_late = p.block < block;
        if (is_late) ++late;
        const uint32_t offset = is_late || frames == 0 ? 0 : (p.offset < frames ? p.offset : frames - 1);
        int64_t slot = -1;
        const bool bypass = p.param == kBypassParam;
        if (!finite_bits(p.value) || p.instance >= m.instance_count ||
            (!bypass && (slot = param_slot(m, p.instance, p.param)) < 0) ||
            !deliver(p.instance, p.param, offset, p.value)) {
            ++rejected;
            continue;
        }
        if (bypass) v.applied_bypass[p.instance].store(p.value, std::memory_order_release);
        else v.applied[slot].store(p.value, std::memory_order_release);
    }
    // Loaded BEFORE the table is read (M3): a write landing during the read
    // bumps it again and is caught next block.
    const uint32_t seq = h.values_seq.load(std::memory_order_acquire);
    if (seq != v.seen_seq) {
        v.seen_seq = seq;
        for (uint32_t i = 0; i < m.instance_count; ++i) {
            for (uint32_t k = 0; k < m.count[i]; ++k) {
                const int64_t slot = param_slot(m, i, k);
                if (slot < 0) break;
                const float x = table[slot].load(std::memory_order_relaxed);
                if (same_bits(x, v.applied[slot].load(std::memory_order_relaxed))) continue;
                if (finite_bits(x) && replace(i, k, x)) v.applied[slot].store(x, std::memory_order_release);
                else ++rejected;
            }
            const float b = h.bypass[i].load(std::memory_order_relaxed);
            if (!same_bits(b, v.applied_bypass[i].load(std::memory_order_relaxed))) {
                if (finite_bits(b) && replace(i, kBypassParam, b))
                    v.applied_bypass[i].store(b, std::memory_order_release);
                else ++rejected;
            }
        }
    }
    if (late) h.params_late.fetch_add(late, std::memory_order_relaxed);
    if (rejected) h.params_rejected.fetch_add(rejected, std::memory_order_relaxed);
    return consumed;
}

}  // namespace sonora::plugin
