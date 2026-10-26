// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/latency_probe.h"

#include <cmath>
#include <cstdio>

namespace sonora {

namespace {

std::string dbfs(float v) {
    if (!(v > 0.0f)) return "silence";
    char text[32];
    std::snprintf(text, sizeof text, "%.1f dBFS", 20.0 * std::log10(static_cast<double>(v)));
    return text;
}

}  // namespace

LatencyProbe::LatencyProbe(uint32_t sample_rate)
    : sample_rate_(sample_rate),
      // A start and a cancel in flight at once, with room to spare.
      requests_(std::make_unique<RingBuffer<Request>>(8)),
      // One measurement at a time: its result, and nothing else, is in it.
      results_(std::make_unique<RingBuffer<Result>>(4)) {}

bool LatencyProbe::start(const Settings& settings, std::string* error) {
    const auto refuse = [error](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (running_) return refuse("a latency measurement is already running");
    Settings s = settings;
    if (s.window == 0) s.window = sample_rate_ / 4;
    if (s.pings == 0 || s.pings > kMaxPings) {
        return refuse("a measurement takes 1 to " + std::to_string(kMaxPings) + " pings, not " +
                      std::to_string(s.pings));
    }
    if (s.window == 0 || s.window > sample_rate_) {
        return refuse("the longest round trip looked for must be 1 frame to one second, not " +
                      std::to_string(s.window) + " frames");
    }
    if (!(s.level > 0.0f) || s.level > 1.0f) {
        return refuse("the impulse level must be above 0 and at most 1 (0 dBFS)");
    }
    const Request r{0, s.output, s.input, s.pings, s.window, s.level};
    if (requests_->write(&r, 1) != 1) {
        return refuse("the audio thread has not taken the last latency requests yet");
    }
    running_ = true;
    return true;
}

void LatencyProbe::cancel() {
    if (!running_) return;
    const Request r{1, 0, 0, 0, 0, 0.0f};
    // Full only with requests the audio thread has not taken, and the start
    // among them is then cancelled by the next call.
    requests_->write(&r, 1);
}

bool LatencyProbe::poll(Result& out) {
    if (results_->read(&out, 1) != 1) return false;
    running_ = false;
    return true;
}

std::string LatencyProbe::describe(const Result& r, uint32_t sample_rate) {
    const auto ms = [sample_rate](uint32_t frames) {
        char text[32];
        std::snprintf(text, sizeof text, "%.2f ms",
                      sample_rate ? 1000.0 * frames / sample_rate : 0.0);
        return std::string(text);
    };
    const std::string heard = "arrival " + dbfs(r.peak) + ", floor " + dbfs(r.noise);
    switch (r.outcome) {
        case Outcome::Measured:
            return "round trip " + std::to_string(r.frames) + " frames (" + ms(r.frames) + "), " +
                   std::to_string(r.pings) + " pings within " + std::to_string(r.spread) +
                   " frame(s), " + heard;
        case Outcome::NoSignal:
            return "no arrival heard (" + heard +
                   "): is the output cabled to the input, and the input's gain up?";
        case Outcome::Unstable:
            return "the pings disagree by " + std::to_string(r.spread) + " frames (" +
                   ms(r.spread) + "): the loop is not a cable, or the interface's clock is"
                   " not stable";
        case Outcome::Discontinuity:
            return "the audio server lost frames " + std::to_string(r.restarts) +
                   " times during the measurement";
        case Outcome::NoPort:
            return "the probe's output or input is not one the backend has";
        case Outcome::Cancelled:
            return "cancelled";
    }
    return "unknown outcome";
}

LatencyProbe::Result LatencyProbe::summary(Outcome outcome) const noexcept SONORA_RT {
    Result r{};
    r.outcome = outcome;
    r.pings = ping_;
    r.restarts = restarts_;
    r.noise = noise_;
    if (ping_ == 0) return r;
    // Few enough for an insertion sort, on a copy so the pings keep their
    // order.
    uint32_t sorted[kMaxPings] = {};
    float quietest = peak_[0];
    for (uint32_t k = 0; k < ping_; ++k) {
        const uint32_t v = lag_[k];
        uint32_t j = k;
        for (; j > 0 && sorted[j - 1] > v; --j) sorted[j] = sorted[j - 1];
        sorted[j] = v;
        if (peak_[k] < quietest) quietest = peak_[k];
    }
    r.frames = sorted[ping_ / 2];
    r.spread = sorted[ping_ - 1] - sorted[0];
    r.peak = quietest;
    return r;
}

void LatencyProbe::finish(const Result& r) noexcept SONORA_RT {
    // One measurement at a time, so the ring always has room; a full one
    // would only lose the result, never wait.
    results_->write(&r, 1);
    phase_ = Phase::Idle;
}

void LatencyProbe::evaluate() noexcept SONORA_RT {
    Result r = summary(Outcome::Measured);
    if (r.peak < kMinPeak || r.peak < kMinSnr * noise_) {
        r.outcome = Outcome::NoSignal;
    } else if (r.spread > kMaxSpread) {
        r.outcome = Outcome::Unstable;
    }
    finish(r);
}

void LatencyProbe::process(const backend::AudioBlock& block) noexcept SONORA_RT {
    Request req{};
    while (requests_->read(&req, 1) == 1) {
        if (req.cancel != 0) {
            if (phase_ != Phase::Idle) finish(summary(Outcome::Cancelled));
            continue;
        }
        run_.output = req.output;
        run_.input = req.input;
        run_.pings = req.pings;
        run_.window = req.window;
        run_.level = req.level;
        phase_ = Phase::Quiet;
        pos_ = 0;
        ping_ = 0;
        pings_heard_.store(0, std::memory_order_relaxed);
        restarts_ = 0;
        have_time_ = false;
        floor_ = 0.0f;
        noise_ = 0.0f;
    }
    if (phase_ == Phase::Idle) return;
    if (run_.output >= block.output_count || run_.input >= block.input_count) {
        finish(summary(Outcome::NoPort));
        return;
    }
    const uint32_t n = block.nframes;
    if (n == 0) return;
    if (have_time_ && block.frame_time != next_time_) {
        // Frames were lost on the way out or in: this ping's impulse, or its
        // arrival, may be among them. It starts again, after a settling
        // quiet long enough for an impulse already in the loop to arrive
        // unmeasured.
        if (++restarts_ > run_.pings + 2) {
            finish(summary(Outcome::Discontinuity));
            return;
        }
        phase_ = Phase::Settle;
        pos_ = 0;
    }
    have_time_ = true;
    next_time_ = block.frame_time + n;

    float* const out = block.outputs[run_.output];
    const float* const in = block.inputs[run_.input];
    for (uint32_t i = 0; i < n; ++i) {
        const float x = in != nullptr ? in[i] : 0.0f;
        const float a = x < 0.0f ? -x : x;
        switch (phase_) {
            case Phase::Settle:
                out[i] = 0.0f;
                if (++pos_ == run_.window) {
                    phase_ = Phase::Quiet;
                    pos_ = 0;
                    floor_ = 0.0f;
                }
                break;
            case Phase::Quiet:
                out[i] = 0.0f;
                if (a > floor_) floor_ = a;
                if (++pos_ == run_.window) {
                    phase_ = Phase::Listen;
                    pos_ = 0;
                    best_ = 0.0f;
                    best_at_ = 0;
                }
                break;
            case Phase::Listen:
                out[i] = pos_ == 0 ? run_.level : 0.0f;
                // The first of equal peaks: a flat-topped arrival is timed
                // at its leading edge.
                if (a > best_) {
                    best_ = a;
                    best_at_ = pos_;
                }
                if (++pos_ == run_.window) {
                    lag_[ping_] = best_at_;
                    peak_[ping_] = best_;
                    if (floor_ > noise_) noise_ = floor_;
                    pings_heard_.store(++ping_, std::memory_order_relaxed);
                    if (ping_ == run_.pings) {
                        evaluate();
                    } else {
                        phase_ = Phase::Quiet;
                        pos_ = 0;
                        floor_ = 0.0f;
                    }
                }
                break;
            case Phase::Idle:
                // Finished inside this block: the rest is the mix's.
                return;
        }
    }
}

}  // namespace sonora
