// SPDX-License-Identifier: GPL-3.0-or-later
#include "synthetic_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace sonora::ui {

namespace {

uint64_t steady_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

constexpr double kPi = 3.14159265358979323846;

double db_to_linear(double db) { return std::pow(10.0, db / 20.0); }

// After this long without a heartbeat the engine counts as gone. §4.1 leaves
// the figure to M4; a quarter second is six UI ticks, long enough not to
// flicker and short enough that a frozen meter is never on screen for long.
constexpr uint64_t kHeartbeatTimeoutNs = 250'000'000;

}  // namespace

const std::vector<std::string>& SyntheticEngine::engine_commands() {
    // engine/control/src/session_commands.cpp, 2026-10-12.
    static const std::vector<std::string> ids = {
        "transport.toggle_record", "transport.record", "transport.stop",
        "track.toggle_arm",        "track.arm_all",    "track.disarm_all",
    };
    return ids;
}

SyntheticEngine::SyntheticEngine(uint32_t tracks, Clock clock)
    : clock_(clock ? std::move(clock) : Clock(steady_ns)) {
    info_.sample_rate = 48000;
    info_.buffer_frames = 128;
    info_.track_count = tracks;
    info_.implemented_commands = engine_commands();
    armed_.assign(tracks, false);
    last_read_ns_.assign(tracks, 0);
}

uint64_t SyntheticEngine::now() const { return clock_(); }

bool SyntheticEngine::connect(std::string& error) {
    if (info_.track_count == 0) {
        error = "the synthetic engine was asked for zero tracks";
        return false;
    }
    start_ns_ = now();
    std::fill(last_read_ns_.begin(), last_read_ns_.end(), start_ns_);
    connected_ = true;
    return true;
}

// Each channel gets a level of its own, so a meter showing a neighbour's
// signal is visible, the way the engine's channel-identity check makes it
// audible. Channels 1-4 have drum-like transients, the rest a slow swell,
// and channel 10 reaches full scale every seven seconds so the over
// indicator has something to latch.
float SyntheticEngine::level_at(uint32_t ch, double t) {
    const double base_db = -42.0 + 2.0 * static_cast<double>(ch % 18);
    double db;
    if (ch < 4) {
        const double period = 0.45 + 0.1 * ch;
        const double phase = std::fmod(t + 0.07 * ch, period) / period;
        db = base_db + 24.0 * std::exp(-phase * 9.0) - 6.0;
    } else {
        db = base_db + 6.0 * std::sin(2.0 * kPi * (0.15 + 0.03 * ch) * t + ch);
    }
    if (ch == 9) {
        const double phase = std::fmod(t, 7.0);
        if (phase < 0.05) db = 0.4;  // a little over full scale, as a float can be
    }
    return static_cast<float>(db_to_linear(db));
}

float SyntheticEngine::take_peak(uint32_t index) {
    if (index >= info_.track_count) return 0.0f;
    const uint64_t t1 = now();
    if (t1 >= death_ns_) return 0.0f;
    const uint64_t t0 = last_read_ns_[index];
    last_read_ns_[index] = t1;
    // The engine's peak is a maximum over every sample since the last read.
    // Sampling the envelope once a millisecond is close enough to that for a
    // display, and it is the property worth keeping: a transient between two
    // ticks still shows.
    float peak = 0.0f;
    const double b = static_cast<double>(t1 - start_ns_) * 1e-9;
    // A reader that comes back after a long gap gets the last second, not a
    // loop over hours of envelope on the GUI thread.
    const double a = std::max(static_cast<double>(t0 - start_ns_) * 1e-9, b - 1.0);
    for (double t = a; t <= b; t += 0.001) peak = std::max(peak, level_at(index, t));
    return std::max(peak, level_at(index, b));
}

bool SyntheticEngine::engine_alive(uint64_t now_ns) const {
    if (!connected_) return false;
    if (now_ns < death_ns_) return true;
    return now_ns - death_ns_ < kHeartbeatTimeoutNs;
}

TransportSnapshot SyntheticEngine::transport() const {
    TransportSnapshot s;
    s.state = mode_;
    s.take = take_;
    if (mode_ == TransportMode::Recording) {
        const uint64_t t = std::min(now(), death_ns_);
        s.playhead_frames = (t - record_start_ns_) * info_.sample_rate / 1'000'000'000ull;
    }
    return s;
}

TrackSnapshot SyntheticEngine::track(uint32_t index) const {
    TrackSnapshot t;
    if (index < armed_.size()) t.armed = armed_[index];
    return t;
}

uint64_t SyntheticEngine::send(const std::string& command, const CommandContext& ctx) {
    CommandOutcome out;
    out.seq = ++seq_;
    out.command = command;
    if (now() >= death_ns_) return out.seq;  // a dead engine never replies
    if (find_command(command) == nullptr) {
        out.result = DispatchResult::UnknownCommand;
    } else {
        const auto& ids = engine_commands();
        if (std::find(ids.begin(), ids.end(), command) == ids.end()) {
            out.result = DispatchResult::NotImplemented;
        } else {
            out.result = handle(command, ctx, out.message) ? DispatchResult::Ok
                                                           : DispatchResult::Failed;
        }
    }
    replies_.push_back(std::move(out));
    return seq_;
}

void SyntheticEngine::poll(std::vector<CommandOutcome>& replies) {
    while (!replies_.empty()) {
        replies.push_back(std::move(replies_.front()));
        replies_.pop_front();
    }
}

bool SyntheticEngine::handle(const std::string& command, const CommandContext& ctx,
                             std::string& why) {
    const bool recording = mode_ == TransportMode::Recording;
    const auto start = [this]() {
        mode_ = TransportMode::Recording;
        record_start_ns_ = now();
        return true;
    };
    const auto stop = [this]() {
        if (mode_ == TransportMode::Stopped) return true;
        mode_ = TransportMode::Stopped;
        ++take_;
        return true;
    };
    if (command == "transport.toggle_record") return recording ? stop() : start();
    if (command == "transport.record") return recording ? true : start();
    if (command == "transport.stop") return stop();

    // Same order of refusals as engine/control: a missing or impossible
    // target is named before the take is.
    const auto take_running = [recording](std::string& reason) {
        if (recording) reason = "arming cannot change during a take; stop first";
        return recording;
    };
    if (command == "track.arm_all" || command == "track.disarm_all") {
        if (take_running(why)) return false;
        std::fill(armed_.begin(), armed_.end(), command == "track.arm_all");
        return true;
    }
    if (command == "track.toggle_arm") {
        if (!ctx.has_target()) {
            why = "no track given, and there is no selection yet";
            return false;
        }
        const auto idx = static_cast<std::size_t>(ctx.target);
        if (idx >= armed_.size()) {
            why = "there is no track " + std::to_string(idx + 1);
            return false;
        }
        if (take_running(why)) return false;
        armed_[idx] = !armed_[idx];
        return true;
    }
    why = "not handled";
    return false;
}

}  // namespace sonora::ui
