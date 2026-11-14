// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/edge_ramp.h"

namespace sonora {

namespace {

bool sums_strips(NodeKind kind) noexcept SONORA_RT {
    return kind == NodeKind::AuxSum || kind == NodeKind::BusSum || kind == NodeKind::PathOut;
}

}  // namespace

void EdgeRamps::allocate(uint32_t tracks, uint32_t buses, uint32_t ramp_frames) {
    tracks_ = tracks;
    strips_ = tracks + buses;
    ramp_frames_ = ramp_frames != 0 ? ramp_frames : 1;
    inv_ramp_ = 1.0f / static_cast<float>(ramp_frames_);
    routes_.reset(new Route[strips_ + 1]());
    fades_.reset(new Fade[strips_ + 1]());
    done_.reset(new uint32_t[strips_ + 1]());
    out_edge_.reset(new uint32_t[strips_ + 1]());
    ghosts_.reset(new Ghost[strips_ + 1]());
    ghost_count_ = 0;
}

void EdgeRamps::clear() noexcept SONORA_RT {
    for (uint32_t s = 0; s < strips_; ++s) {
        routes_[s] = Route{};
        fades_[s] = Fade{};
        done_[s] = 0;
    }
    ghost_count_ = 0;
}

uint32_t EdgeRamps::strip_of(const PlaybackPlan& plan, const PlanEdge& edge) const noexcept SONORA_RT {
    if (edge.send != kNoSend || !sums_strips(plan.nodes[edge.to_node].kind)) return kNone;
    const PlanNode& from = plan.nodes[edge.from_node];
    switch (from.kind) {
        case NodeKind::TrackOut:
        case NodeKind::TrackFader: {
            if (from.track >= plan.track_count) return kNone;
            const uint32_t t = plan.tracks[from.track].engine_track;
            return t < tracks_ ? t : kNone;
        }
        case NodeKind::AuxFader:
            return from.track < strips_ - tracks_ ? tracks_ + from.track : kNone;
        default:
            return kNone;
    }
}

uint32_t EdgeRamps::find_node(const PlaybackPlan& plan, uint32_t stable_id) const noexcept SONORA_RT {
    for (uint32_t ni = 0; ni < plan.node_count; ++ni) {
        if (plan.nodes[ni].stable_id == stable_id) return ni;
    }
    return kNone;
}

bool EdgeRamps::line_taken(const PlaybackPlan& plan, uint32_t line) const noexcept SONORA_RT {
    for (uint32_t ei = 0; ei < plan.edge_count; ++ei) {
        if (plan.edges[ei].delay_frames != 0 && plan.edges[ei].delay_line == line) return true;
    }
    return false;
}

bool EdgeRamps::place_ghost(const PlaybackPlan& plan, const Fade& f, uint32_t from, uint32_t delay_lines,
                            PlanEdge& out) const noexcept SONORA_RT {
    const uint32_t to = find_node(plan, f.ghost_to_id);
    // A destination the walk reaches before the strip would sum the ghost
    // before the strip has played this chunk.
    if (to == kNone || to <= from || !sums_strips(plan.nodes[to].kind)) return false;
    uint32_t delay = f.ghost_delay;
    uint32_t line = f.ghost_line;
    // The old edge played undelayed when the pool had no line for it.
    if (delay != 0 && line >= delay_lines) {
        delay = 0;
        line = kNoDelayLine;
    }
    // The line now serves another edge: the frames in it are no longer the
    // strip's, and summing them twice would be wrong too.
    if (delay != 0 && line_taken(plan, line)) return false;
    out = PlanEdge{from, to, delay, delay != 0 ? line : kNoDelayLine, kNoSend};
    return true;
}

void EdgeRamps::adopt_strip(const PlaybackPlan& plan, uint32_t s, uint32_t delay_lines) noexcept SONORA_RT {
    Route& r = routes_[s];
    Fade& f = fades_[s];
    const uint32_t ei = out_edge_[s];
    if (ei == kNone) {
        r = Route{};
        f = Fade{};
        return;
    }
    const PlanEdge& e = plan.edges[ei];
    const uint32_t from_id = plan.nodes[e.from_node].stable_id;
    const uint32_t to_id = plan.nodes[e.to_node].stable_id;
    // Moved again mid-fade, or no longer the same strip: the running fade
    // is dropped, and a new one starts from where the strip last went.
    if (f.on && (f.from_id != from_id || f.to_id != to_id)) f = Fade{};
    if (!f.on && r.known && r.from_id == from_id && r.to_id != to_id) {
        Fade next;
        next.on = true;
        next.from_id = from_id;
        next.to_id = to_id;
        next.ghost = true;
        next.ghost_to_id = r.to_id;
        next.ghost_delay = r.delay_frames;
        next.ghost_line = r.delay_line;
        PlanEdge g;
        // With no ghost the old route stops here whatever the new one does,
        // and a fade-in would only add a dip to the cut: the swap stays one.
        if (place_ghost(plan, next, e.from_node, delay_lines, g)) {
            f = next;
            done_[s] = 0;
        }
    }
    if (f.on) {
        f.edge = ei;
        PlanEdge g;
        if (f.ghost && place_ghost(plan, f, e.from_node, delay_lines, g)) {
            ghosts_[ghost_count_++] = Ghost{g, s};
        } else {
            f.ghost = false;
            if (done_[s] >= ramp_frames_) f = Fade{};
        }
    }
    r = Route{from_id, to_id, e.delay_frames, e.delay_line, true};
}

void EdgeRamps::adopt(const PlaybackPlan& plan, uint32_t delay_lines) noexcept SONORA_RT {
    ghost_count_ = 0;
    for (uint32_t s = 0; s < strips_; ++s) out_edge_[s] = kNone;
    for (uint32_t ei = 0; ei < plan.edge_count; ++ei) {
        const uint32_t s = strip_of(plan, plan.edges[ei]);
        if (s != kNone) out_edge_[s] = ei;
    }
    for (uint32_t s = 0; s < strips_; ++s) adopt_strip(plan, s, delay_lines);
}

uint32_t EdgeRamps::fading_in(const PlaybackPlan& plan, uint32_t ei) const noexcept SONORA_RT {
    const uint32_t s = strip_of(plan, plan.edges[ei]);
    if (s == kNone) return kNone;
    const Fade& f = fades_[s];
    return f.on && f.edge == ei && done_[s] < ramp_frames_ ? s : kNone;
}

void EdgeRamps::advance(uint32_t n) noexcept SONORA_RT {
    for (uint32_t s = 0; s < strips_; ++s) {
        Fade& f = fades_[s];
        if (!f.on) continue;
        const uint64_t end = uint64_t{ramp_frames_} + (f.ghost ? f.ghost_delay : 0);
        const uint64_t done = uint64_t{done_[s]} + n;
        if (done >= end) {
            f = Fade{};
            done_[s] = 0;
        } else {
            done_[s] = static_cast<uint32_t>(done);
        }
    }
    uint32_t kept = 0;
    for (uint32_t k = 0; k < ghost_count_; ++k) {
        if (fades_[ghosts_[k].strip].on) ghosts_[kept++] = ghosts_[k];
    }
    ghost_count_ = kept;
}

}  // namespace sonora
