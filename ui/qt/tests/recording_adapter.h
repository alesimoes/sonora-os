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
    };

    explicit RecordingAdapter(uint32_t tracks = 18,
                              std::vector<std::string> implemented = {}) {
        info_.sample_rate = 48000;
        info_.buffer_frames = 128;
        info_.track_count = tracks;
        info_.implemented_commands = std::move(implemented);
        peaks.assign(tracks, 0.0f);
    }

    bool connect(std::string&) override { return true; }
    const EngineInfo& info() const override { return info_; }
    uint64_t send(const std::string& command, const CommandContext& ctx) override {
        sent.push_back({command, ctx.target});
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
    TransportSnapshot transport() const override { return snapshot; }
    TrackSnapshot track(uint32_t i) const override {
        TrackSnapshot t;
        t.armed = i < armed.size() && armed[i];
        return t;
    }
    float take_peak(uint32_t i) override {
        ++peak_reads;
        if (i >= peaks.size()) return 0.0f;
        const float p = peaks[i];
        peaks[i] = 0.0f;  // the reader's cursor moves on, as StateView's does
        return p;
    }
    bool engine_alive(uint64_t) const override { return alive; }

    std::vector<Sent> sent;
    std::vector<float> peaks;
    std::vector<bool> armed;
    TransportSnapshot snapshot;
    bool alive = true;
    int peak_reads = 0;
    DispatchResult next_result = DispatchResult::Ok;
    std::string next_message;

private:
    EngineInfo info_;
    uint64_t seq_ = 0;
    std::vector<CommandOutcome> pending_;
};

}  // namespace sonora::ui::test
