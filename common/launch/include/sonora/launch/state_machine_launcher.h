#pragma once

// Reference Launcher (ADR-0164): a quantised state machine per track column.
// Each column holds the playing scene and at most one pending action (start
// a scene, or stop) with the frame it lands on. A new command on the column
// replaces the pending action; so a column yields at most a stop and a start
// per block, and the transition storage is sized 2 x tracks at creation.

#include "sonora/launch/launch_command_queue.h"
#include "sonora/launch/launcher.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
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
    std::uint64_t overruns() const override { return overruns_.load(std::memory_order_relaxed); }

    void set_transport(const TransportSnapshot& t) override { transport_ = t; }
    TransitionList advance(std::int64_t block_start, std::uint32_t frames) override;
    const LaunchGrid& grid() const override { return grid_; }

private:
    enum class Pending : std::uint8_t { None, Start, Stop };
    struct Column {
        std::int32_t playing = kNone;
        // The clip that started: a later assign_clip on the cell does not
        // rename the one already sounding.
        ClipId playing_clip = kNoClip;
        std::int32_t pending_scene = kNone;
        Pending pending = Pending::None;
        std::int64_t pending_frame = 0;
    };

    bool enqueue(const Command& c);
    void apply(const Command& c, std::int64_t now);
    void queue_start(std::uint32_t track, std::uint32_t scene, std::int64_t at);
    void queue_stop(std::uint32_t track, std::int64_t at);
    void fire_due(std::uint32_t track, std::int64_t block_start, std::uint32_t frames);
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
    TransportSnapshot transport_;
    Quantize quantize_;
};

}  // namespace sonora::launch
