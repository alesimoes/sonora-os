// SPDX-License-Identifier: GPL-3.0-or-later
//
// An EngineAdapter that records what it is sent and answers Ok, for tests of
// *routing*: which command, with which target, reached the engine. It holds
// no engine behaviour, so no test of the interface passes because a stand-in
// engine happened to agree with it. Engine behaviour is FakeEngine's (M4).
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "engine_adapter.h"

namespace sonora::ui::test {

class RecordingAdapter final : public EngineAdapter {
public:
    struct Sent {
        std::string command;
        int target;
        std::string argument;
    };

    explicit RecordingAdapter(uint32_t tracks = 18,
                              std::vector<std::string> implemented = {}) {
        info_.sample_rate = 48000;
        info_.buffer_frames = 128;
        info_.track_count = tracks;
        info_.implemented_commands = std::move(implemented);
        peaks.assign(tracks, 0.0f);
    }

    // As a new Hello would change them.
    void set_capabilities(std::vector<std::string> caps) { info_.capabilities = std::move(caps); }

    bool connect(std::string&) override { return true; }
    const EngineInfo& info() const override { return info_; }
    uint64_t send(const std::string& command, const CommandContext& ctx) override {
        sent.push_back({command, ctx.target, ctx.argument});
        CommandOutcome o;
        o.seq = ++seq_;
        o.command = command;
        o.result = next_result;
        o.message = next_message;
        pending_.push_back(o);
        return seq_;
    }
    void poll(std::vector<CommandOutcome>& replies) override {
        for (auto& o : pending_) replies.push_back(std::move(o));
        pending_.clear();
    }
    void take_events(std::vector<EngineEvent>& out) override {
        for (auto& e : events) out.push_back(std::move(e));
        events.clear();
    }
    void frame(EngineFrame& out) const override {
        ++frames;
        out.transport = snapshot;
        out.tracks.assign(info_.track_count, TrackSnapshot{});
        for (uint32_t i = 0; i < info_.track_count; ++i) {
            out.tracks[i].armed = i < armed.size() && armed[i];
        }
    }
    float take_peak(uint32_t i) override {
        ++peak_reads;
        if (i >= peaks.size()) return 0.0f;
        const float p = peaks[i];
        peaks[i] = 0.0f;  // the reader's cursor moves on, as StateView's does
        return p;
    }
    bool engine_alive(uint64_t) const override { return alive; }
    std::string link_error() const override { return link_error_text; }
    uint64_t send_track_param(uint32_t track, api::ParamId param, float value,
                              api::Gesture gesture) override {
        gain_changes.push_back({track, value, param, gesture});
        return ++seq_;
    }
    uint64_t send_send_level(uint64_t send, float linear) override {
        level_changes.push_back({send, false, linear});
        return ++seq_;
    }
    uint64_t send_aux_gain(uint64_t bus, float linear) override {
        level_changes.push_back({bus, true, linear});
        return ++seq_;
    }
    uint64_t send_plugin_parameter(uint64_t insert, const std::string& key, float value) override {
        plugin_changes.push_back({insert, key, value});
        return ++seq_;
    }
    bool take_catalog(api::PluginCatalog& out) override {
        if (!catalog_new) return false;
        out = catalog;
        catalog_new = false;
        return true;
    }

    struct GainChange {
        uint32_t track;
        float linear;
        api::ParamId param = api::ParamId::Gain;
        api::Gesture gesture = api::Gesture::Move;
    };
    std::vector<GainChange> gain_changes;
    struct LevelChange {
        uint64_t id;
        bool aux;
        float linear;
    };
    std::vector<LevelChange> level_changes;

    struct PluginChange {
        uint64_t insert;
        std::string key;
        float value;
    };
    std::vector<PluginChange> plugin_changes;
    api::PluginCatalog catalog;
    bool catalog_new = false;

    std::vector<Sent> sent;
    std::vector<float> peaks;
    std::vector<bool> armed;
    TransportSnapshot snapshot;
    std::vector<EngineEvent> events;   // handed over at the next take_events()
    mutable int frames = 0;
    bool alive = true;
    std::string link_error_text;
    int peak_reads = 0;
    DispatchResult next_result = DispatchResult::Ok;
    std::string next_message;

private:
    EngineInfo info_;
    uint64_t seq_ = 0;
    std::vector<CommandOutcome> pending_;
};

}  // namespace sonora::ui::test
