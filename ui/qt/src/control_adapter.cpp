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

uint64_t ControlAdapter::submitModel(const api::ModelRequest& request) {
    const uint64_t seq = engine_.submit(request);
    model_seqs_.insert(seq);
    return seq;
}

void ControlAdapter::poll(std::vector<CommandOutcome>& replies) {
    inbox_.clear();
    engine_.poll(inbox_);
    model_replies_.clear();
    for (const api::CommandReply& r : inbox_.replies) {
        if (model_seqs_.erase(r.seq) != 0) {
            model_replies_.push_back(r);
            continue;
        }
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
    for (const api::EngineEvent& e : inbox_.events) {
        EngineEvent o;
        switch (e.kind) {
            case api::EventKind::Notice: o.kind = EngineEventKind::Notice; break;
            case api::EventKind::InputsMissing: o.kind = EngineEventKind::InputsMissing; break;
            case api::EventKind::TakeFailed: o.kind = EngineEventKind::TakeFailed; break;
            case api::EventKind::Fatal: o.kind = EngineEventKind::Fatal; break;
        }
        o.take = e.frame;
        o.text = e.text;
        events_.push_back(std::move(o));
    }
    if (model_sink_) model_sink_(inbox_.model, model_replies_);
}

void ControlAdapter::take_events(std::vector<EngineEvent>& out) {
    for (EngineEvent& e : events_) out.push_back(std::move(e));
    events_.clear();
}

void ControlAdapter::frame(EngineFrame& out) const {
    engine_.state().frame(state_);
    const api::TransportSnapshot& s = state_.transport;
    TransportSnapshot& t = out.transport;
    t.state = s.state == api::TransportState::Recording ? TransportMode::Recording
              : s.state == api::TransportState::Rolling ? TransportMode::Rolling
                                                        : TransportMode::Stopped;
    t.playhead_frames = s.playhead;
    t.take = s.take;
    t.xruns = s.xruns;
    t.health_flags = ((s.health_flags & api::kHealthServerGone) ? kHealthServerGone : 0u) |
                     ((s.health_flags & api::kHealthNoInputs) ? kHealthNoInputs : 0u) |
                     ((s.health_flags & api::kHealthTakeFailed) ? kHealthTakeFailed : 0u);
    t.stale = s.stale;
    out.tracks.resize(state_.tracks.size());
    for (std::size_t i = 0; i < state_.tracks.size(); ++i) {
        const api::TrackSnapshot& a = state_.tracks[i];
        out.tracks[i].armed = a.armed();
        out.tracks[i].muted = a.muted();
        out.tracks[i].overruns = a.overruns;
    }
}

float ControlAdapter::take_peak(uint32_t index) { return engine_.state().take_peak(index); }

bool ControlAdapter::engine_alive(uint64_t now_ns) const {
    return engine_.state().engine_alive(now_ns);
}

}  // namespace sonora::ui
