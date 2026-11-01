// SPDX-License-Identifier: GPL-3.0-or-later
#include "control_adapter.h"

namespace sonora::ui {

static_assert(kInsertSlotCount == api::kInsertSlots,
              "the interface's insert slots are the state block's");

bool ControlAdapter::connect(std::string& error) {
    if (!engine_.connect(error)) return false;
    const api::Hello& h = engine_.hello();
    info_.sample_rate = h.sample_rate;
    info_.buffer_frames = h.buffer_frames;
    info_.track_count = h.track_count;
    info_.implemented_commands = h.implemented_commands;
    info_.capabilities = h.capabilities;
    if (probe_) connection_ = probe_();
    return true;
}

void ControlAdapter::watchConnection(ConnectionProbe probe, LinkSink sink) {
    probe_ = std::move(probe);
    link_sink_ = std::move(sink);
    connection_ = probe_ ? probe_() : 0;
}

uint64_t ControlAdapter::send(const std::string& command, const CommandContext& ctx) {
    api::CommandRequest r;
    r.command = command;
    r.target = ctx.target;
    r.argument = ctx.argument;
    const uint64_t seq = engine_.send(r);
    await(seq, command, false);
    return seq;
}

void ControlAdapter::await(uint64_t seq, std::string name, bool live) {
    pending_[seq] = Awaited{std::move(name), live};
    // Seqs only grow, so the first is the oldest.
    if (pending_.size() > kMaxAwaiting) pending_.erase(pending_.begin());
}

uint64_t ControlAdapter::submitModel(const api::ModelRequest& request) {
    const uint64_t seq = engine_.submit(request);
    model_seqs_.insert(seq);
    if (model_seqs_.size() > kMaxAwaiting) model_seqs_.erase(model_seqs_.begin());
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
            o.command = it->second.name;
            o.live = it->second.live;
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
            // Shown on its insert slot, by id; an empty text clears it.
            case api::EventKind::PluginFault: o.kind = EngineEventKind::PluginFault; break;
            case api::EventKind::RecoveryOffer: o.kind = EngineEventKind::RecoveryOffer; break;
            case api::EventKind::SessionFolder: o.kind = EngineEventKind::SessionFolder; break;
            case api::EventKind::Latency: o.kind = EngineEventKind::Latency; break;
        }
        o.take = e.frame;
        o.insert = e.insert;
        o.text = e.text;
        o.flags = e.flags;
        events_.push_back(std::move(o));
    }
    if (!inbox_.catalogs.empty()) {
        catalog_ = std::move(inbox_.catalogs.back());
        catalog_new_ = true;
    }
    if (model_sink_) model_sink_(inbox_.model, model_replies_);

    // After the sink, so the replies the old connection did bring count.
    const uint64_t now = probe_ ? probe_() : connection_;
    if (now == connection_) return;
    connection_ = now;
    model_seqs_.clear();
    for (const auto& [seq, awaited] : pending_) {
        CommandOutcome o;
        o.seq = seq;
        o.command = awaited.name;
        o.live = awaited.live;
        o.result = DispatchResult::Failed;
        o.message = "the connection to the engine was lost before it answered";
        replies.push_back(std::move(o));
    }
    pending_.clear();
    if (now != 0) {
        // A new connection is a new Hello, possibly from another build or
        // an engine restarted on another interface or buffer size.
        const api::Hello& h = engine_.hello();
        info_.sample_rate = h.sample_rate;
        info_.buffer_frames = h.buffer_frames;
        info_.track_count = h.track_count;
        info_.implemented_commands = h.implemented_commands;
    info_.capabilities = h.capabilities;
    }
    if (link_sink_) link_sink_(now != 0);
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
                     ((s.health_flags & api::kHealthTakeFailed) ? kHealthTakeFailed : 0u) |
                     ((s.health_flags & api::kHealthPluginsDisabled) ? kHealthPluginsDisabled : 0u);
    t.compensation_frames = s.compensation_frames;
    t.compensation_shortfall_frames = s.compensation_shortfall_frames;
    t.click = (s.transport_flags & api::kTransportClick) != 0;
    t.count_off = (s.transport_flags & api::kTransportCountOff) != 0;
    t.counting_off = (s.transport_flags & api::kTransportCountingOff) != 0;
    t.locating = (s.transport_flags & api::kTransportLocating) != 0;
    t.record_ready = (s.transport_flags & api::kTransportRecordReady) != 0;
    t.playback_misses = s.playback_misses;
    t.stale = s.stale;
    out.tracks.resize(state_.tracks.size());
    for (std::size_t i = 0; i < state_.tracks.size(); ++i) {
        const api::TrackSnapshot& a = state_.tracks[i];
        TrackSnapshot& o = out.tracks[i];
        o.armed = a.armed();
        o.muted = a.muted();
        o.overruns = a.overruns;
        o.latency_frames = a.latency_frames;
        for (uint32_t slot = 0; slot < kInsertSlotCount; ++slot) {
            const uint32_t f = a.inserts[slot].flags;
            o.inserts[slot].flags =
                ((f & api::kInsertPresent) ? kSlotPresent : 0u) |
                ((f & api::kInsertActive) ? kSlotActive : 0u) |
                ((f & api::kInsertBypassed) ? kSlotBypassed : 0u) |
                ((f & api::kInsertRunning) ? kSlotRunning : 0u) |
                ((f & api::kInsertRestarting) ? kSlotRestarting : 0u) |
                ((f & api::kInsertFaulted) ? kSlotFaulted : 0u) |
                ((f & api::kInsertMissing) ? kSlotMissing : 0u);
            o.inserts[slot].latency_frames = a.inserts[slot].latency_frames;
        }
    }
}

uint64_t ControlAdapter::send_track_param(uint32_t track, api::ParamId param, float value,
                                          api::Gesture gesture) {
    api::ParameterChange c;
    c.track = track;
    c.param = param;
    c.value = value;
    c.gesture = gesture;
    const uint64_t seq = engine_.send(c);
    // Named for the status line, so a refusal says which control.
    await(seq, std::string(param == api::ParamId::Pan ? "the pan of track " : "the fader of track ") +
                   std::to_string(track + 1), true);
    return seq;
}

uint64_t ControlAdapter::send_send_level(uint64_t send, float linear) {
    api::ParameterChange c;
    c.param = api::ParamId::SendLevel;
    c.id = send;
    c.value = linear;
    const uint64_t seq = engine_.send(c);
    await(seq, "the fader of a send", true);
    return seq;
}

uint64_t ControlAdapter::send_aux_gain(uint64_t bus, float linear) {
    api::ParameterChange c;
    c.param = api::ParamId::AuxGain;
    c.id = bus;
    c.value = linear;
    const uint64_t seq = engine_.send(c);
    await(seq, "the fader of an aux bus", true);
    return seq;
}

uint64_t ControlAdapter::send_plugin_parameter(uint64_t insert, const std::string& key,
                                               float value) {
    api::PluginParameterChange c;
    c.insert = insert;
    c.key = key;
    c.value = value;
    const uint64_t seq = engine_.send(c);
    // Named for the status line, as a command is, so a refusal says what.
    await(seq, "plugin parameter " + key, true);
    return seq;
}

bool ControlAdapter::take_catalog(api::PluginCatalog& out) {
    if (!catalog_new_) return false;
    out = catalog_;
    catalog_new_ = false;
    return true;
}

float ControlAdapter::take_peak(uint32_t index) { return engine_.state().take_peak(index); }

float ControlAdapter::take_bus_peak(uint32_t side) {
    return engine_.state().take_bus_peak(side);
}

bool ControlAdapter::engine_alive(uint64_t now_ns) const {
    return engine_.state().engine_alive(now_ns);
}

}  // namespace sonora::ui
