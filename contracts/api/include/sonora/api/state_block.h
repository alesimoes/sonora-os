// SPDX-License-Identifier: GPL-3.0-or-later
//
// The shared state block (ADR-0007 §4.1 and its 2026-10-12 update, item 3).
//
// Fixed size, created once per session. One writer, the engine's publisher
// thread, which runs on a fixed timer and never on the audio thread. Any
// number of readers, which after M7 map it read-only in another process.
// So everything a reader touches is a lock-free, address-free std::atomic no
// wider than 8 bytes (update item 2); a value that has to be read together
// with another sits under the seqlock.
//
// Layout: one StateHeader, then capacity_tracks TrackState entries, each on
// its own cache line. state_block_bytes() gives the size to allocate.
//
// Peaks are not under the seqlock. Each publish writes the peak since the
// previous publish into a ring slot tagged with the publish tick, so every
// reader keeps a cursor of its own and takes the maximum of the slots it has
// not seen (state_view.h). Two readers never steal each other's peaks, which
// Track::take_peak()'s exchange would do: the publisher is its only caller.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

namespace sonora::api {

// Bump on any change to the layout below, and to what a field means.
constexpr uint32_t kStateLayoutVersion = 1;
constexpr uint32_t kStateMagic = 0x534F4E53;   // "SONS"

// Publish ticks a peak survives in the ring. At the fastest pace measured
// clean (1 kHz) that is 64 ms, three times a 60 Hz reader's period; at the
// default 100 Hz it is 640 ms.
constexpr uint32_t kPeakSlots = 64;

enum class TransportState : uint32_t { Stopped = 0, Rolling = 1, Recording = 2 };

enum TrackFlag : uint32_t {
    kTrackArmed = 1u << 0,
    kTrackMuted = 1u << 1,
    kTrackSolo = 1u << 2,
    kTrackInputMonitor = 1u << 3,
    kTrackInputConnected = 1u << 4,
};

enum HealthFlag : uint32_t {
    kHealthServerGone = 1u << 0,   // the backend is fatal; see the Fatal event
    kHealthNoInputs = 1u << 1,     // no capture input is connected
    kHealthTakeFailed = 1u << 2,   // the open take stopped writing
};

// A slot packs the publish tick's low 32 bits above the peak's float bits,
// so tag and value are one 8-byte atomic and a reader never pairs one
// tick's tag with another tick's value.
struct PeakRing {
    // Not "slots": Qt defines that word as a macro, and ui/qt includes this.
    std::atomic<uint64_t> entries[kPeakSlots];
};

struct alignas(64) TrackState {
    // Under the header's seqlock.
    std::atomic<uint32_t> flags;
    std::atomic<float> gain;             // linear
    std::atomic<uint32_t> overruns;      // capture frames lost on this track
    std::atomic<uint32_t> clip_count;    // monotonic; a reader compares, never clears
    std::atomic<uint32_t> latency_frames;
    // Not under the seqlock; see above.
    PeakRing peaks;
};

struct alignas(64) StateHeader {
    // Written once before the block is handed to any reader.
    uint32_t magic;
    uint32_t layout_version;
    uint32_t capacity_tracks;
    uint32_t peak_slots;
    uint64_t publish_period_ns;
    uint64_t block_bytes;

    std::atomic<uint32_t> track_count;
    // Publish ticks, starting at 1. A reader that sees it stand still for
    // long enough says the engine is not answering (StateView::engine_alive).
    // Also the tick the peak ring slots are tagged with.
    std::atomic<uint64_t> heartbeat;
    // Odd while a publish is writing. Covers everything below and every
    // TrackState field except the peaks.
    std::atomic<uint64_t> seq;

    std::atomic<uint32_t> transport_state;   // TransportState
    std::atomic<int32_t> take;               // the open take, or the next one
    std::atomic<uint64_t> playhead;          // frames
    std::atomic<uint32_t> xruns;
    std::atomic<uint32_t> overruns;          // all tracks
    std::atomic<uint32_t> clock_steps;       // backend clock discontinuities
    std::atomic<uint32_t> health_flags;      // HealthFlag
    std::atomic<uint32_t> inputs_connected;
    std::atomic<uint32_t> buffer_frames;     // 0 until the backend's first period
};

// Shared memory needs atomics that never fall back to libatomic's lock
// table: a lock there would be a lock between processes, and one held by a
// reader that died would stall the engine's publisher for ever.
static_assert(std::atomic<uint32_t>::is_always_lock_free, "uint32_t atomics in the state block");
static_assert(std::atomic<int32_t>::is_always_lock_free, "int32_t atomics in the state block");
static_assert(std::atomic<uint64_t>::is_always_lock_free, "uint64_t atomics in the state block");
static_assert(std::atomic<float>::is_always_lock_free, "float atomics in the state block");
static_assert(sizeof(std::atomic<uint64_t>) == 8, "no atomic wider than 8 bytes in shared memory");
static_assert(sizeof(float) == sizeof(uint32_t), "a peak slot packs a float into 32 bits");

constexpr std::size_t state_header_bytes() {
    return (sizeof(StateHeader) + 63) / 64 * 64;
}

constexpr std::size_t state_block_bytes(uint32_t capacity_tracks) {
    return state_header_bytes() + std::size_t{capacity_tracks} * sizeof(TrackState);
}

// Anonymous memory for a block, 64-byte aligned: the in-process home of
// the state block until M7 puts it in a memfd. Allocated once, at session
// creation, and zeroed by StateWriter::init.
class StateBlockMemory {
public:
    StateBlockMemory() = default;
    explicit StateBlockMemory(uint32_t capacity_tracks)
        : bytes_(state_block_bytes(capacity_tracks)),
          data_(::operator new(bytes_, std::align_val_t{64})) {}
    ~StateBlockMemory() { release(); }
    StateBlockMemory(const StateBlockMemory&) = delete;
    StateBlockMemory& operator=(const StateBlockMemory&) = delete;
    StateBlockMemory(StateBlockMemory&& o) noexcept : bytes_(o.bytes_), data_(o.data_) {
        o.data_ = nullptr;
        o.bytes_ = 0;
    }
    StateBlockMemory& operator=(StateBlockMemory&& o) noexcept {
        if (this != &o) {
            release();
            bytes_ = o.bytes_;
            data_ = o.data_;
            o.data_ = nullptr;
            o.bytes_ = 0;
        }
        return *this;
    }

    void* data() { return data_; }
    const void* data() const { return data_; }
    std::size_t bytes() const { return bytes_; }

private:
    void release() {
        if (data_ != nullptr) ::operator delete(data_, std::align_val_t{64});
        data_ = nullptr;
    }
    std::size_t bytes_ = 0;
    void* data_ = nullptr;
};

// What one publish writes, apart from the peaks.
struct TransportFields {
    TransportState state = TransportState::Stopped;
    int32_t take = 0;
    uint64_t playhead = 0;
    uint32_t xruns = 0;
    uint32_t overruns = 0;
    uint32_t clock_steps = 0;
    uint32_t health_flags = 0;
    uint32_t inputs_connected = 0;
    uint32_t buffer_frames = 0;
};

struct TrackFields {
    uint32_t flags = 0;
    float gain = 1.0f;
    uint32_t overruns = 0;
    uint32_t clip_count = 0;
    uint32_t latency_frames = 0;
};

// The writer's side. Not a thread: the publisher (engine/control) and
// FakeEngine call it from their own. One writer per block, ever.
class StateWriter {
public:
    // Constructs the block in `memory` (state_block_bytes(capacity) bytes,
    // 64-byte aligned) and leaves it valid and empty: heartbeat 0, nothing
    // published. Returns false, writing nothing, if the memory is too small
    // or misaligned.
    bool init(void* memory, std::size_t bytes, uint32_t capacity_tracks,
              uint32_t track_count, uint64_t publish_period_ns);

    // One publish: the seqlock section, then the peaks, then the heartbeat.
    // `tracks` and `peaks` have track_count entries. Short and bounded: it
    // never loops and never waits for a reader.
    void publish(const TransportFields& transport, const TrackFields* tracks,
                 const float* peaks);

    // A publish that stops after opening the seqlock, for the test that a
    // reader gives up and says "stale" instead of spinning. finish_write()
    // closes it.
    void begin_write_for_test();
    void finish_write_for_test();

    uint64_t ticks() const { return tick_; }
    uint32_t track_count() const { return track_count_; }

private:
    StateHeader* header_ = nullptr;
    TrackState* tracks_ = nullptr;
    uint32_t track_count_ = 0;
    uint64_t tick_ = 0;
};

// Where the TrackState array starts in a block.
inline TrackState* state_tracks(void* block) {
    return reinterpret_cast<TrackState*>(static_cast<unsigned char*>(block) +
                                         state_header_bytes());
}
inline const TrackState* state_tracks(const void* block) {
    return reinterpret_cast<const TrackState*>(static_cast<const unsigned char*>(block) +
                                               state_header_bytes());
}

}  // namespace sonora::api
