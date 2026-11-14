// SPDX-License-Identifier: GPL-3.0-or-later
//
// A strip rerouted at a plan swap crossfades from its old destination to its
// new one instead of cutting (ADR-0202). The old edge lives on as a ghost:
// the strip's signal, faded out at the input of the delay line it had, summed
// into the old destination until that line has played out. The new edge
// fades in at the input of its own, fresh line. Both fades are applied before
// the lines, so two routes aligned by PDC cross on the same output frame.
//
// Sized when the session is created; everything after allocate() runs on
// the audio thread and never allocates, locks or throws.
#pragma once

#include <cstdint>
#include <memory>

#include "sonora/playback_plan.h"
#include "sonora/rt.h"

namespace sonora {

// A reroute's fade, in milliseconds of the session's rate: long enough not
// to click, short enough that an edit is heard at once. Never a count of
// blocks: the output would then depend on how the backend cuts its
// callbacks (ADR-0202 §2).
constexpr uint32_t kEdgeRampMs = 10;

class EdgeRamps {
public:
    static constexpr uint32_t kNone = 0xFFFFFFFFu;

    // `tracks` engine tracks and `buses` engine buses are the strips; a fade
    // lasts `ramp_frames`. Allocates: never on the audio thread.
    void allocate(uint32_t tracks, uint32_t buses, uint32_t ramp_frames);
    uint32_t ramp_frames() const { return ramp_frames_; }

    // At a swap, over a plan that passed the session's bounds check: starts
    // a fade for every strip whose output edge now ends elsewhere, and
    // finds again, by stable id, the fades already running. `delay_lines`:
    // the session's pool, past which a line is none.
    void adopt(const PlaybackPlan& plan, uint32_t delay_lines) noexcept SONORA_RT;
    // Every fade and every route forgotten: the next swap starts none.
    void clear() noexcept SONORA_RT;

    // The strip whose output edge `ei` of the plan is fading in, or kNone.
    uint32_t fading_in(const PlaybackPlan& plan, uint32_t ei) const noexcept SONORA_RT;
    // The fade-in's gain at frame i of the current chunk; the ghost's is
    // one less it.
    float gain(uint32_t strip, uint32_t i) const noexcept SONORA_RT {
        const uint64_t at = uint64_t{done_[strip]} + i + 1;
        return at >= ramp_frames_ ? 1.0f : static_cast<float>(at) * inv_ramp_;
    }

    // The ghosts summed into node `ni`, by index k < ghost_count().
    uint32_t ghost_count() const noexcept SONORA_RT { return ghost_count_; }
    const PlanEdge& ghost_edge(uint32_t k) const noexcept SONORA_RT { return ghosts_[k].edge; }
    uint32_t ghost_strip(uint32_t k) const noexcept SONORA_RT { return ghosts_[k].strip; }

    // After each chunk of `n` frames: the fades move on, and a ghost whose
    // line has played out is dropped.
    void advance(uint32_t n) noexcept SONORA_RT;

    // The strip an edge leaves from when it is that strip's output (a
    // TrackOut, TrackFader or AuxFader into a summing node), or kNone.
    uint32_t strip_of(const PlaybackPlan& plan, const PlanEdge& edge) const noexcept SONORA_RT;

private:
    // A strip's output edge as the last adopted plan had it.
    struct Route {
        uint32_t from_id = 0;
        uint32_t to_id = 0;
        uint32_t delay_frames = 0;
        uint32_t delay_line = 0;
        bool known = false;
    };
    // A strip's fade in progress. The fade-in ends at ramp_frames; the
    // ghost lasts its line's delay longer, playing out the frames in it.
    struct Fade {
        bool on = false;
        uint32_t from_id = 0;
        uint32_t to_id = 0;        // the new destination
        uint32_t edge = kNone;     // the fading-in edge's index in the plan
        bool ghost = false;
        uint32_t ghost_to_id = 0;  // the old destination
        uint32_t ghost_delay = 0;
        uint32_t ghost_line = 0;
    };
    struct Ghost {
        PlanEdge edge;
        uint32_t strip = 0;
    };

    uint32_t find_node(const PlaybackPlan& plan, uint32_t stable_id) const noexcept SONORA_RT;
    bool line_taken(const PlaybackPlan& plan, uint32_t line) const noexcept SONORA_RT;
    // The ghost of fade `f` in this plan, from node `from`; false if it
    // cannot be placed (its destination is gone or comes first, or its line
    // went to another edge).
    bool place_ghost(const PlaybackPlan& plan, const Fade& f, uint32_t from, uint32_t delay_lines,
                     PlanEdge& out) const noexcept SONORA_RT;
    void adopt_strip(const PlaybackPlan& plan, uint32_t s, uint32_t delay_lines) noexcept SONORA_RT;

    uint32_t tracks_ = 0;
    uint32_t strips_ = 0;
    uint32_t ramp_frames_ = 1;
    float inv_ramp_ = 1.0f;
    std::unique_ptr<Route[]> routes_;
    std::unique_ptr<Fade[]> fades_;
    std::unique_ptr<uint32_t[]> done_;       // frames into each strip's fade
    std::unique_ptr<uint32_t[]> out_edge_;   // per strip: its output edge in the plan
    std::unique_ptr<Ghost[]> ghosts_;
    uint32_t ghost_count_ = 0;
};

}  // namespace sonora
