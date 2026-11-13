#pragma once

// Reference Launcher (ADR-0164): a quantised state machine per track column.
// Each column holds the playing scene, the recording scene, and at most one
// pending action (start a scene, stop, or start a capture) with the frame it
// lands on. A new command on the column replaces the pending action. A
// column yields at most one ending (Stop or RecordStop) and one beginning
// (Start or RecordStart) per block, so the transition storage is sized
// 2 x tracks at creation.

#include "sonora/launch/launch_command_queue.h"
#include "sonora/launch/launcher.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace sonora::launch {

struct LauncherConfig {
    std::uint32_t tracks = 8;
    std::uint32_t scenes = 8;
    std::size_t queue_capacity = 256;
    Quantize quantize = Quantize::Bar;
};

class StateMachineLauncher final : public Launcher {
public:
    explicit StateMachineLauncher(const LauncherConfig& config);

    bool assign_clip(std::uint32_t track, std::uint32_t scene, ClipId clip) override;
    bool set_quantize(Quantize q) override;
    bool trigger_cell(std::uint32_t track, std::uint32_t scene) override;
    bool trigger_scene(std::uint32_t scene) override;
    bool stop_track(std::uint32_t track) override;
    bool stop_all() override;
    bool arm_record(std::uint32_t track, std::uint32_t scene, ClipId clip,
                    std::uint32_t bars) override;
    std::uint64_t overruns() const override { return overruns_.load(std::memory_order_relaxed); }

    void set_transport(const TransportSnapshot& t) override;
    TransitionList advance(std::int64_t block_start, std::uint32_t frames) override;
    const LaunchGrid& grid() const override { return grid_; }

    // The producer reads this to stamp arrivals; a lock-free atomic or the
    // no-lock claim on push() does not hold.
    bool clock_is_lock_free() const { return published_.is_lock_free(); }

private:
    static constexpr std::int64_t kUnknown = std::numeric_limits<std::int64_t>::min();

    enum class Pending : std::uint8_t { None, Start, Stop, Record };
    struct Column {
        std::int32_t playing = kNone;
        // The clip that started: a later assign_clip on the cell does not
        // rename the one already sounding.
        ClipId playing_clip = kNoClip;
        std::int32_t recording = kNone;
        ClipId recording_clip = kNoClip;
        Pending pending = Pending::None;
        std::int32_t pending_scene = kNone;
        std::int64_t pending_frame = 0;
        Quantize pending_quantize = Quantize::Immediate;
        // false for a fixed-length capture end: a grid change keeps it.
        bool pending_resnaps = false;
        ClipId pending_clip = kNoClip;
        std::uint32_t pending_bars = 0;
    };

    bool enqueue(Command c);
    void apply(const Command& c, std::int64_t now);
    void queue_start(std::uint32_t track, std::uint32_t scene, std::int64_t at);
    void queue_stop(std::uint32_t track, std::int64_t at);
    void queue_record(const Command& c, std::int64_t at);
    void set_pending(std::uint32_t track, Pending p, std::int32_t scene, std::int64_t at);
    void fire_due(std::uint32_t track, std::int64_t block_start, std::uint32_t frames);
    void end_capture(std::uint32_t track, std::uint32_t offset);
    void begin_capture(std::uint32_t track, std::int64_t frame, std::uint32_t offset);
    void emit(Transition::Kind kind, std::uint32_t track, std::int32_t scene, ClipId clip,
              std::uint32_t offset);
    void clear_marks(std::uint32_t track);
    void mark(std::uint32_t track);

    LaunchGrid grid_;
    std::vector<Column> columns_;
    std::vector<Transition> transitions_;
    std::uint32_t transition_count_ = 0;
    std::uint32_t dropped_ = 0;
    CommandQueue queue_;
    std::atomic<std::uint64_t> overruns_{0};
    // End of the last block advanced: written by the audio thread, read by
    // the producer to stamp arrivals.
    std::atomic<std::int64_t> published_{kUnknown};
    std::int64_t clock_ = kUnknown;
    TransportSnapshot transport_;
    Quantize quantize_;
};

}  // namespace sonora::launch
