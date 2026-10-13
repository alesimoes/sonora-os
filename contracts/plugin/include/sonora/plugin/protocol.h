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
// Block protocol. The audio crosses once per block per host process (§4.3).
//
//  Engine, block N (RT):
//    1. If completed_block != submitted_block, the host is still busy with an
//       older block. Do NOT write the input planes; drop this block for the
//       host (count it) and go to step 5. This rule is what makes one input
//       and one output slot sufficient: the engine never writes a slot the
//       host may be reading, and never reads one the host may be writing.
//    2. Write input planes, then submitted_frames, then
//       submitted_block.store(N, release).
//    3. request_word.fetch_add(1, release), then wake the host (the seam).
//    4. Collect: the engine reads output planes only after it has observed
//       completed_block == N with acquire. It never waits past its deadline.
//       pipeline_depth == 0: collect in this same callback, bounded by the
//         deadline (whether a bounded wait is allowed on the audio thread is
//         the auditor's ruling; a poll-only engine uses depth 1).
//       pipeline_depth == 1: collect block N at the start of block N + 1,
//         with no wait at all. One block of delay, declared as latency.
//    5. A block not collected in time is dropped: the engine outputs what
//       its policy says (silence, or dry) and counts it. Repeated misses
//       remove the host from the plan; current_instance says which plugin.
//
//  Host, its own thread:
//    a. Read request_word, then submitted_block (acquire). If it is ahead of
//       the last block processed, process it; else sleep on request_word
//       with a timeout and bump heartbeat on every pass, idle or not.
//    b. Drain the parameter ring, run the chain, write output planes.
//    c. completed_block.store(N, release); done_word.fetch_add(1, release);
//       wake done_word (only useful to an engine doing a bounded wait).
//
// Latency: latency_frames is the whole chain's latency PLUS
// pipeline_depth * block_frames (§4.3: IPC delay is declared like any
// look-ahead, one mechanism for PDC). It may change while running (a plugin
// changing its look-ahead); latency_generation is bumped after every change,
// and the engine re-plans off the audio thread when it sees a new value.
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
    NotFound = 1,             // no plugin with that URI is installed
    ChannelMismatch = 2,      // declared audio ports cannot map to `channels`
    UnsupportedFeature = 3,   // requires an LV2 feature the host lacks
    UnsupportedPort = 4,      // a port type the host cannot connect (CV, ...)
    InstantiateFailed = 5,
    WroteOutsideBuffers = 6,  // declared N channels, wrote more (canary)
    NonFiniteOutput = 7,      // NaN or Inf from silence during validation
    TooManyInstances = 8,
};

// Engine -> host parameter change. `instance` is the index in the chain,
// `port` the plugin's own port index (LV2 port index). Applied by the host
// at the start of the next block it processes.
struct ParamChange {
    uint32_t instance;
    uint32_t port;
    float value;
    uint32_t reserved;
};

// Host -> engine, one per chain instance. Written by the host only.
struct InstanceReport {
    std::atomic<uint32_t> latency_frames;  // this plugin's own, no IPC part
    std::atomic<uint32_t> replicas;        // mono plugin run once per channel
    std::atomic<uint64_t> last_run_ns;
    std::atomic<uint64_t> worst_run_ns;    // worst case, never an average
};

struct ChannelHeader {
    // --- fixed: written by the engine before the host starts, then read-only
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

    // --- host -> engine
    alignas(kCacheLine) std::atomic<uint64_t> completed_block;
    std::atomic<uint32_t> done_word;     // an engine doing a bounded wait sleeps here
    std::atomic<uint32_t> state;         // HostState
    std::atomic<uint64_t> heartbeat;     // bumped at least every 100 ms
    std::atomic<uint32_t> latency_frames;      // chain + pipeline_depth * block_frames
    std::atomic<uint32_t> latency_generation;  // bumped after latency_frames changes
    std::atomic<int32_t> current_instance;     // chain index being run, -1 between
    std::atomic<uint32_t> refusal;             // Refusal
    std::atomic<int32_t> refusal_instance;     // which chain index, -1 if none
    std::atomic<uint32_t> host_flags;          // HostFlag bits
    std::atomic<uint64_t> worst_block_ns;      // host-side, whole chain

    InstanceReport instances[kMaxInstances];
};

enum HostFlag : uint32_t {
    kHostRealtime = 1u << 0,      // got SCHED_FIFO at host_rt_priority
    kHostMemoryLocked = 1u << 1,  // mlockall succeeded
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

// Channel c of a plane. `region` is the mapped base address.
inline float* input_plane(void* region, const ChannelHeader& h, uint32_t c) {
    return reinterpret_cast<float*>(static_cast<unsigned char*>(region) + h.input_offset) +
           std::size_t(c) * h.max_frames;
}
inline float* output_plane(void* region, const ChannelHeader& h, uint32_t c) {
    return reinterpret_cast<float*>(static_cast<unsigned char*>(region) + h.output_offset) +
           std::size_t(c) * h.max_frames;
}

// The engine side's half of initialisation. The region must be zeroed and at
// least region_size(channels, max_frames) bytes. Not RT: done at creation.
inline ChannelHeader* init_header(void* region, uint32_t sample_rate, uint32_t max_frames,
                                  uint32_t block_frames, uint32_t channels,
                                  uint32_t pipeline_depth, uint32_t instance_count,
                                  int32_t host_rt_priority) {
    auto* h = new (region) ChannelHeader{};
    h->magic = kChannelMagic;
    h->layout_version = kLayoutVersion;
    h->sample_rate = sample_rate;
    h->max_frames = max_frames;
    h->block_frames = block_frames;
    h->channels = channels;
    h->pipeline_depth = pipeline_depth;
    h->instance_count = instance_count;
    h->host_rt_priority = host_rt_priority;
    h->input_offset = input_offset();
    h->output_offset = output_offset(channels, max_frames);
    h->region_size = region_size(channels, max_frames);
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

// Parameter ring, engine side. RT-safe: no allocation, no wait; a full ring
// returns false and the caller counts it. Single producer only.
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
