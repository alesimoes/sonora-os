#pragma once

// The clip launcher contract (ADR-0164). The control methods are called by
// one producer thread (the UI or control thread) and only enqueue a fixed-size
// command; advance() runs on the audio thread, applies the queued commands
// and reports the clip starts and stops that fall inside the block.

#include "sonora/launch/launch_grid.h"
#include "sonora/launch/launch_quantize.h"

#include <cstdint>

namespace sonora::launch {

struct Transition {
    enum class Kind : std::uint8_t { Start, Stop };
    Kind kind = Kind::Start;
    std::uint32_t track = 0;
    std::uint32_t scene = 0;
    ClipId clip = kNoClip;
    // Frames from block_start; always < the block's frame count.
    std::uint32_t offset = 0;
};

// Fixed capacity, set at creation. A track yields at most a stop and a start
// per block, so 2 x tracks never overflows; dropped stays as a guard.
struct TransitionList {
    const Transition* items = nullptr;
    std::uint32_t count = 0;
    std::uint32_t dropped = 0;
    const Transition& operator[](std::uint32_t i) const { return items[i]; }
};

class Launcher {
public:
    virtual ~Launcher() = default;

    // Producer thread. false: the index is out of the grid, or the queue is
    // full (counted in overruns()). Nothing waits.
    virtual bool assign_clip(std::uint32_t track, std::uint32_t scene, ClipId clip) = 0;
    virtual bool set_quantize(Quantize q) = 0;
    virtual bool trigger_cell(std::uint32_t track, std::uint32_t scene) = 0;
    virtual bool trigger_scene(std::uint32_t scene) = 0;
    virtual bool stop_track(std::uint32_t track) = 0;
    virtual bool stop_all() = 0;
    virtual std::uint64_t overruns() const = 0;

    // Audio thread. The snapshot is used for the commands applied by the
    // following advance() calls.
    virtual void set_transport(const TransportSnapshot& t) = 0;
    // Audio thread: no allocation, lock or throw. The list stays valid until
    // the next call.
    virtual TransitionList advance(std::int64_t block_start, std::uint32_t frames) = 0;
    // Audio thread only: the grid as of the last advance().
    virtual const LaunchGrid& grid() const = 0;
};

}  // namespace sonora::launch
