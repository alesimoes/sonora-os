// SPDX-License-Identifier: GPL-3.0-or-later
#include "control_adapter.h"

namespace sonora::ui {

bool ControlAdapter::connect(std::string& error) {
    if (!engine_.connect(error)) return false;
    const api::Hello& h = engine_.hello();
    info_.sample_rate = h.sample_rate;
    info_.buffer_frames = h.buffer_frames;
    info_.track_count = h.track_count;
    info_.implemented_commands = h.implemented_commands;
    return true;
}

uint64_t ControlAdapter::send(const std::string& command, const CommandContext& ctx) {
    api::CommandRequest r;
    r.command = command;
    r.target = ctx.target;
    r.argument = ctx.argument;
    const uint64_t seq = engine_.send(r);
    pending_[seq] = command;
    return seq;
}

void ControlAdapter::poll(std::vector<CommandOutcome>& replies) {
    inbox_.clear();
    engine_.poll(inbox_);
    for (const api::CommandReply& r : inbox_.replies) {
        CommandOutcome o;
        o.seq = r.seq;
        const auto it = pending_.find(r.seq);
        if (it != pending_.end()) {
            o.command = it->second;
            pending_.erase(it);
        }
        o.result = r.result;
        o.message = r.message;
        replies.push_back(std::move(o));
    }
    // Model updates are for the Edit window's replica, which still runs on
    // its local document (timeline_document.h); it subscribes here when it
    // moves onto the engine's.
}

TransportSnapshot ControlAdapter::transport() const {
    const api::TransportSnapshot s = engine_.state().transport();
    TransportSnapshot t;
    t.state = s.state == api::TransportState::Recording ? TransportMode::Recording
              : s.state == api::TransportState::Rolling ? TransportMode::Rolling
                                                        : TransportMode::Stopped;
    t.playhead_frames = s.playhead;
    t.take = s.take;
    t.xruns = s.xruns;
    t.health_flags = ((s.health_flags & api::kHealthServerGone) ? kHealthServerGone : 0u) |
                     ((s.health_flags & api::kHealthNoInputs) ? kHealthNoInputs : 0u);
    t.stale = s.stale;
    return t;
}

TrackSnapshot ControlAdapter::track(uint32_t index) const {
    const api::TrackSnapshot s = engine_.state().track(index);
    TrackSnapshot t;
    t.armed = s.armed();
    t.muted = s.muted();
    t.overruns = s.overruns;
    return t;
}

float ControlAdapter::take_peak(uint32_t index) { return engine_.state().take_peak(index); }

bool ControlAdapter::engine_alive(uint64_t now_ns) const {
    return engine_.state().engine_alive(now_ns);
}

}  // namespace sonora::ui
