// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// ============================================================================
//  DRAFT CONTRACT PROPOSAL. NOT ACCEPTED. DO NOT BUILD ENGINE CODE ON IT YET.
//
//  Proposed by plugin-engineer for rt-engineer review (ADR-0007 §4.3, §6:
//  contracts/plugin is owned jointly by plugin-engineer and rt-engineer).
//  The engine half (engine/plugin_client, M4 onwards) does not exist; the
//  only consumers are plugins/host and its test driver, which plays the
//  engine's role. Every field, ordering rule and the version number may
//  change in review. kLayoutVersion stays 0 while this is a draft.
//
//  Merged as a draft (PR #26, engine owner's review of 2026-10-13). What
//  still blocks ACCEPTANCE, also in planning/BACKLOG.md, Track 4:
//
//  TODO(accept) Parameters: changes tagged with the block they belong to and
//    a frame offset inside it, so a change lands on a known sample, plus a
//    last-value table in the region, so a dropped block or a restarted host
//    never loses the current value of a control.
//  TODO(accept) Parameter values: ParamChange::value is an LV2 port value
//    but a NORMALISED VST3 value, so the engine must know each plugin's
//    format. Format-neutral values need the host to convert plain to
//    normalised, which in VST3 is a controller call and must happen off the
//    processing thread (brain/research/plugin-host-vst3.md).
//  TODO(accept) Crash and restart: a region a dead host touched is never
//    reused. Restart = new memfd, new host, wait for Ready, re-read
//    latency_frames, then rejoin through a control-thread plan swap with
//    every parameter replayed from the last-value table.
//  TODO(accept) Plan swaps (ADR-0009): bypass is an atomic plus an RtEvent;
//    HostChannel state (block counters, miss streak) is keyed by the node's
//    stable id and lives outside the plan, so a swap does not reset it; an
//    in-flight depth-1 block is discarded across a chain swap; PDC across a
//    latency_generation bump keeps the old alignment until the re-planned
//    graph is swapped in; a "dry" miss plays the dry signal delayed by
//    latency_frames, never the undelayed dry, which would jump in time.
//  TODO(accept) Versioning: a header_bytes field, static_asserts on sizeof
//    and offsetof of every field, and how a host reports "incompatible"
//    (today: exit code 2 and stderr, before it writes anything).
// ============================================================================
//
// One shared-memory region per plugin host process. The engine creates it,
// sizes it at session creation, initialises the "fixed" part and passes it to
// the host. Layout, in order:
//
//   ChannelHeader                      (this file)
//   input planes   [channels][max_frames] float     engine writes, host reads
//   output planes  [channels][max_frames] float     host writes, engine reads
//
// Platform-neutral on purpose (ADR-0008): fixed-width integers, std::atomic
// and offsets only. How the region is created and passed (memfd + SCM_RIGHTS,
// a section object), and how a sleeping side is woken, live in a platform
// seam, never here.
//
// ---------------------------------------------------------------------------
// Trust. The host runs third-party code and maps the whole region writable,
// so a scribbling plugin can change ANY word of it, the "fixed" part
// included. Rules for the engine:
//
//  - Every word the host can write is untrusted: completed_block,
//    latency_frames, latency_generation, current_instance, refusal,
//    refusal_instance, state, host_flags, heartbeat, param_read,
//    params_rejected, the instance reports, the output planes' contents,
//    and the fixed part.
//  - Such a word is bounds-checked before use (checked_instance,
//    checked_latency, checked_state below) and is NEVER turned into a
//    pointer, an offset or an index without that check.
//  - Plane addresses come from the engine's private Layout (make_layout),
//    computed from its own configuration, never from the header's
//    input_offset / output_offset / max_frames, which exist for the host.
//  - The parameter ring index is masked, so a forged param_read can only
//    make the ring look full (pushes refused and counted), never move a
//    store outside the ring.
//
// The host, in turn, validates the header it was handed (header_valid) and
// builds its own Layout once from the validated values (layout_of).
//
// ---------------------------------------------------------------------------
// Block protocol. The audio crosses once per block per host process (§4.3).
// The audio thread never waits on a host: no timed wait, no futex wait, no
// spin. It submits, and it collects only what is already there.
//
//  Engine, callback N (RT):
//    1. If completed_block != submitted_block, the host is still busy with an
//       older block. Do NOT write the input planes; drop this block for the
//       host (count it) and go to step 5. This rule is what makes one input
//       and one output slot sufficient: the engine never writes a slot the
//       host may be reading, and never reads one the host may be writing.
//    2. pipeline_depth == 1, the default: if the block now completed is the
//       one submitted in callback N - 1, read the output planes; that is
//       this callback's output. A block that completed later than that is
//       discarded, never played late, which would break the declared latency.
//    3. Write input planes, then submitted_frames, then
//       submitted_block.store(N, release); request_word.fetch_add(1, release),
//       then wake the host through the seam: one FUTEX_WAKE, whose use on the
//       audio thread awaits the RT auditor's ruling.
//    4. pipeline_depth == 0 is poll-only, pending the same ruling: submit at
//       the start of the callback, process the rest of the graph, then read
//       completed_block ONCE (acquire). Equal to N: read the output planes.
//       Otherwise the block is dropped. Nothing waits in between.
//    5. A block not collected is dropped: the engine outputs what its policy
//       says (silence, or dry delayed by latency_frames) and counts it.
//       Repeated misses remove the host from the plan; current_instance,
//       checked, says which plugin.
//
//  Host, its own thread:
//    a. Read request_word, then submitted_block (acquire). If it is ahead of
//       the last block processed, process it; else sleep on request_word
//       with a timeout and bump heartbeat on every pass, idle or not.
//    b. Drain the parameter ring (at most kParamRingCapacity entries; one it
//       cannot apply is counted in params_rejected), run the chain, write
//       output planes.
//    c. completed_block.store(N, release); done_word.fetch_add(1, release);
//       wake done_word. Only the control thread (waiting for Ready) and
//       measurement harnesses sleep on done_word, never the audio thread.
//
// Non-finite output. The host replaces every NaN or Inf a plugin writes with
// 0 before the next plugin, or the engine, sees it, and adds the count to
// that plugin's InstanceReport::nonfinite_samples. The engine's control
// thread watches the counters and names the plugin to the operator; whether
// a persistent offender leaves the plan is engine policy. A plugin that
// produces NaN or Inf from silence is refused at validation (NonFiniteOutput).
//
// Denormals. The host sets flush-to-zero and denormals-are-zero on its
// processing thread before the first block; kHostFlushDenormals says it did.
//
// Latency: latency_frames is the whole chain's latency PLUS
// pipeline_depth * block_frames (§4.3: IPC delay is declared like any
// look-ahead, one mechanism for PDC). It may change while running (a plugin
// changing its look-ahead); latency_generation is bumped after every change,
// and the engine re-plans off the audio thread when it sees a new value. A
// value above kMaxLatencyFrames is a fault, not a plan.
// ---------------------------------------------------------------------------

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

namespace sonora::plugin {

inline constexpr uint32_t kChannelMagic = 0x48504E53u;  // "SNPH", little-endian
inline constexpr uint32_t kLayoutVersion = 0;           // 0 = draft, unstable
inline constexpr uint32_t kMaxInstances = 16;           // per host process
inline constexpr uint32_t kParamRingCapacity = 256;     // power of two
inline constexpr std::size_t kCacheLine = 64;
inline constexpr uint32_t kDefaultPipelineDepth = 1;
// Above this a reported latency is corrupt: 2^20 frames is 21.8 s at 48 kHz,
// far past any look-ahead a mixing plugin needs.
inline constexpr uint32_t kMaxLatencyFrames = 1u << 20;
inline constexpr uint32_t kLatencyInvalid = 0xFFFFFFFFu;

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
};

// Engine -> host parameter change. `instance` is the index in the chain,
// `port` the plugin's own parameter: the LV2 port index, or the VST3
// ParamID. `value` is the LV2 port value, or the VST3 NORMALISED value (0..1,
// clamped by the host). Applied by the host at the start of the next block
// it processes: the first block submitted after the push. Block-tagged
// changes and format-neutral values are TODO(accept) above.
struct ParamChange {
    uint32_t instance;
    uint32_t port;
    float value;
    uint32_t reserved;
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
    // --- fixed: written by the engine before the host starts. The host can
    // still overwrite it (see Trust); the engine never reads it back.
    uint32_t magic;
    uint32_t layout_version;
    uint32_t sample_rate;
    uint32_t max_frames;       // per block, the size of every plane
    uint32_t block_frames;     // nominal quantum; used for the IPC latency
    uint32_t channels;         // audio channels in and out of the chain
    uint32_t pipeline_depth;   // 0 or 1, see the block protocol
    uint32_t instance_count;   // plugins in the chain, <= kMaxInstances
    int32_t host_rt_priority;  // SCHED_FIFO priority to ask for; 0 = none
    uint32_t reserved0;
    uint64_t input_offset;     // byte offsets from the start of the region
    uint64_t output_offset;
    uint64_t region_size;

    // --- engine -> host
    alignas(kCacheLine) std::atomic<uint64_t> submitted_block;
    std::atomic<uint32_t> submitted_frames;
    std::atomic<uint32_t> request_word;  // the host sleeps on this word
    std::atomic<uint32_t> shutdown;      // non-zero: finish and exit

    // --- parameter ring, SPSC, engine produces and host consumes.
    // Indices run free and are masked; write - read is the fill.
    alignas(kCacheLine) std::atomic<uint32_t> param_write;
    alignas(kCacheLine) std::atomic<uint32_t> param_read;
    alignas(kCacheLine) ParamChange params[kParamRingCapacity];

    // --- host -> engine. Untrusted, every word (see Trust).
    alignas(kCacheLine) std::atomic<uint64_t> completed_block;
    std::atomic<uint32_t> done_word;     // control thread only, never the audio thread
    std::atomic<uint32_t> state;         // HostState
    std::atomic<uint64_t> heartbeat;     // bumped at least every 100 ms
    std::atomic<uint32_t> latency_frames;      // chain + pipeline_depth * block_frames
    std::atomic<uint32_t> latency_generation;  // bumped after latency_frames changes
    std::atomic<int32_t> current_instance;     // chain index being run, -1 between
    std::atomic<uint32_t> refusal;             // Refusal
    std::atomic<int32_t> refusal_instance;     // which chain index, -1 if none
    std::atomic<uint32_t> host_flags;          // HostFlag bits
    std::atomic<uint64_t> worst_block_ns;      // host-side, whole chain
    // Ring entries the host could not apply: no such instance or writable
    // parameter, or a non-finite value. Counted, never silently dropped.
    std::atomic<uint64_t> params_rejected;

    InstanceReport instances[kMaxInstances];
};

enum HostFlag : uint32_t {
    kHostRealtime = 1u << 0,        // got SCHED_FIFO at host_rt_priority
    kHostMemoryLocked = 1u << 1,    // mlockall succeeded
    kHostFlushDenormals = 1u << 2,  // FTZ and DAZ set on the processing thread
};

// Shared memory crosses processes, so every atomic in it must be address-free,
// which for GCC and Clang means always lock-free (ADR-0007 update 1, item 2:
// nothing wider than 8 bytes). The wake seam also treats the 32-bit words as
// plain integers, so their representation must be exactly that.
static_assert(std::atomic<uint32_t>::is_always_lock_free, "u32 atomics must be lock-free");
static_assert(std::atomic<int32_t>::is_always_lock_free, "i32 atomics must be lock-free");
static_assert(std::atomic<uint64_t>::is_always_lock_free, "u64 atomics must be lock-free");
static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t), "wake words are plain u32");
static_assert(alignof(std::atomic<uint32_t>) == alignof(uint32_t), "wake words are plain u32");

constexpr std::size_t align_up(std::size_t n, std::size_t a) { return (n + a - 1) / a * a; }

constexpr std::size_t plane_bytes(uint32_t channels, uint32_t max_frames) {
    return align_up(std::size_t(channels) * max_frames * sizeof(float), kCacheLine);
}

constexpr std::size_t input_offset() { return align_up(sizeof(ChannelHeader), kCacheLine); }

constexpr std::size_t output_offset(uint32_t channels, uint32_t max_frames) {
    return input_offset() + plane_bytes(channels, max_frames);
}

constexpr std::size_t region_size(uint32_t channels, uint32_t max_frames) {
    return output_offset(channels, max_frames) + plane_bytes(channels, max_frames);
}

// Where the planes are. Each side keeps its own copy in private memory,
// computed from values it trusts: the engine from its configuration, the
// host from a header that passed header_valid. Never re-read from the
// region, which the other side, or a plugin, can overwrite.
struct Layout {
    uint32_t channels = 0;
    uint32_t max_frames = 0;
    std::size_t input_offset = 0;
    std::size_t output_offset = 0;
    std::size_t region_size = 0;
};

constexpr Layout make_layout(uint32_t channels, uint32_t max_frames) {
    return Layout{channels, max_frames, input_offset(), output_offset(channels, max_frames),
                  region_size(channels, max_frames)};
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

// The engine side's half of initialisation. The region must be zeroed and at
// least layout.region_size bytes. Not RT: done at creation. The engine keeps
// `layout`; the header's copy of it is for the host.
inline ChannelHeader* init_header(void* region, const Layout& layout, uint32_t sample_rate,
                                  uint32_t block_frames, uint32_t pipeline_depth,
                                  uint32_t instance_count, int32_t host_rt_priority) {
    auto* h = new (region) ChannelHeader{};
    h->magic = kChannelMagic;
    h->layout_version = kLayoutVersion;
    h->sample_rate = sample_rate;
    h->max_frames = layout.max_frames;
    h->block_frames = block_frames;
    h->channels = layout.channels;
    h->pipeline_depth = pipeline_depth;
    h->instance_count = instance_count;
    h->host_rt_priority = host_rt_priority;
    h->input_offset = layout.input_offset;
    h->output_offset = layout.output_offset;
    h->region_size = layout.region_size;
    h->current_instance.store(-1, std::memory_order_relaxed);
    h->refusal_instance.store(-1, std::memory_order_relaxed);
    return h;
}

// The checks a host makes before trusting a region it was handed.
inline bool header_valid(const ChannelHeader& h, std::size_t mapped_size) {
    return h.magic == kChannelMagic && h.layout_version == kLayoutVersion &&
           h.channels > 0 && h.max_frames > 0 && h.block_frames > 0 &&
           h.block_frames <= h.max_frames && h.pipeline_depth <= 1 &&
           h.instance_count <= kMaxInstances &&
           h.input_offset == input_offset() &&
           h.output_offset == output_offset(h.channels, h.max_frames) &&
           h.region_size == region_size(h.channels, h.max_frames) &&
           h.region_size <= mapped_size;
}

// The host's Layout, from a header that passed header_valid. Taken once; the
// host does not re-read the fixed part either.
inline Layout layout_of(const ChannelHeader& h) { return make_layout(h.channels, h.max_frames); }

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

// Parameter ring, engine side. RT-safe: no allocation, no wait; a full ring
// returns false and the caller counts it. Single producer only. param_read is
// host-written: the mask keeps the store inside the ring whatever it says,
// and a value ahead of param_write reads as a full ring.
inline bool push_param(ChannelHeader& h, const ParamChange& p) noexcept {
    const uint32_t w = h.param_write.load(std::memory_order_relaxed);
    const uint32_t r = h.param_read.load(std::memory_order_acquire);
    if (w - r >= kParamRingCapacity) return false;
    h.params[w & (kParamRingCapacity - 1)] = p;
    h.param_write.store(w + 1, std::memory_order_release);
    return true;
}

// Parameter ring, host side. Single consumer only.
inline bool pop_param(ChannelHeader& h, ParamChange& out) noexcept {
    const uint32_t r = h.param_read.load(std::memory_order_relaxed);
    const uint32_t w = h.param_write.load(std::memory_order_acquire);
    if (r == w) return false;
    out = h.params[r & (kParamRingCapacity - 1)];
    h.param_read.store(r + 1, std::memory_order_release);
    return true;
}

}  // namespace sonora::plugin
