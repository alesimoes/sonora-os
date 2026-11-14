// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fixtures shared by the LTC behaviour tests (ADR-0124 §4, §5): a scripted
// engine that publishes the transport and records the commands sent, a paced
// sample source, a stream of consecutive timecode frames and the decoder
// helper. Each behaviour file keeps its own test cases; nothing here reads a
// clock or sleeps.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "fake_ltc_io.h"
#include "sonora/api/engine_control.h"
#include "sonora/api/state_block.h"
#include "sonora/api/state_view.h"
#include "sonora/ltc/ltc.h"
#include "sonora/ltc/ltc_sync.h"

namespace ltc_test {

inline int failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            ++::ltc_test::failures;                                      \
        }                                                                \
    } while (0)

namespace api = sonora::api;
using namespace sonora::ltc;
using api::TransportState;

inline constexpr std::uint32_t kSr = 48'000;
inline constexpr LtcRate kRate = LtcRate::Fps25;
inline constexpr std::size_t kFrameSamples = 1'920;   // 25 fps at 48 kHz, exact

// The engine as the drivers see it: a published transport (the real state
// block and view) and the commands they send.
class ScriptedEngine : public api::EngineControl {
public:
    explicit ScriptedEngine(bool attached = true) : memory_(1) {
        std::string error;
        writer_.init(memory_.data(), memory_.bytes(), 1, 0, 10'000'000);
        if (attached) view_.attach(memory_.data(), memory_.bytes(), error);
        set(TransportState::Stopped, 0);
    }

    void set(TransportState state, std::uint64_t playhead) {
        api::TransportFields t;
        t.state = state;
        t.playhead = playhead;
        writer_.publish(t, nullptr, nullptr);
    }

    bool connect(std::string&) override { return true; }
    const api::Hello& hello() const override { return hello_; }
    std::uint64_t send(const api::CommandRequest& r) override {
        commands.push_back(r);
        return ++seq_;
    }
    std::uint64_t send(const api::ParameterChange&) override { return ++seq_; }
    std::uint64_t send(const api::PluginParameterChange&) override { return ++seq_; }
    std::uint64_t submit(const api::ModelRequest&) override { return ++seq_; }
    void poll(api::Inbox&) override {}
    api::StateView& state() override { return view_; }

    std::size_t count(const char* id) const {
        return static_cast<std::size_t>(
            std::count_if(commands.begin(), commands.end(),
                          [id](const api::CommandRequest& c) { return c.command == id; }));
    }

    std::vector<api::CommandRequest> commands;

private:
    api::StateBlockMemory memory_;
    api::StateWriter writer_;
    api::StateView view_;
    api::Hello hello_;
    std::uint64_t seq_ = 0;
};

// A source the test loads one tick's samples into.
class PacedSource final : public LtcSampleSource {
public:
    void load(const std::vector<float>& samples) {
        pending_.assign(samples.begin(), samples.end());
        pos_ = 0;
    }
    std::size_t read(Span<float> out) override {
        const std::size_t n = std::min(out.size, pending_.size() - pos_);
        std::copy_n(pending_.data() + pos_, n, out.data);
        pos_ += n;
        return n;
    }

private:
    std::vector<float> pending_;
    std::size_t pos_ = 0;
};

inline LtcTime tc(int h, int m, int s, int f, LtcRate rate = kRate) {
    LtcTime t;
    t.h = static_cast<std::uint8_t>(h);
    t.m = static_cast<std::uint8_t>(m);
    t.s = static_cast<std::uint8_t>(s);
    t.frame = static_cast<std::uint8_t>(f);
    t.rate = rate;
    return t;
}

// Consecutive frames of timecode from `start`, the way a master sends them.
class Stream {
public:
    explicit Stream(const LtcTime& start) : enc_(kSr, kRate), next_(start) {}
    std::vector<float> frames(std::size_t n) {
        std::vector<float> out(n * kFrameSamples);
        next_ = enc_.render(next_, Span<float>{out.data(), out.size()});
        return out;
    }

private:
    LtcEncoder enc_;
    LtcTime next_;
};

inline LtcConfig chase_config() {
    LtcConfig c;
    c.mode = LtcMode::Chase;
    c.sample_rate = kSr;
    c.rate = kRate;
    return c;
}

inline LtcConfig generate_config() {
    LtcConfig c = chase_config();
    c.mode = LtcMode::Generate;
    return c;
}

inline std::uint64_t sample_of(const LtcTime& t) { return t.to_samples(kSr); }

struct Heard {
    std::vector<LtcTime> time;
    std::vector<std::size_t> end;
};

inline Heard decode(const std::vector<float>& signal) {
    Heard h;
    LtcDecoder dec(kSr, kRate);
    LtcDecoded out;
    constexpr std::size_t kBlock = 1024;
    for (std::size_t pos = 0; pos < signal.size(); pos += kBlock) {
        const std::size_t n = std::min(kBlock, signal.size() - pos);
        dec.feed(Span<const float>{signal.data() + pos, n}, out);
        for (std::size_t i = 0; i < out.count; ++i) {
            h.time.push_back(out.time[i]);
            h.end.push_back(pos + out.end_offset[i]);
        }
    }
    return h;
}

}  // namespace ltc_test
