// SPDX-License-Identifier: GPL-3.0-or-later
// LTC chase and generate on the control thread (ADR-0124 §4, §5).
//
// The samples come in through LtcSampleSource and go out through
// LtcSampleSink; the audio-thread tap behind them is a later item (§6). The
// drivers speak to the engine through api::EngineControl only.
//
// LtcChase is the policy, pure and inline: fed the engine's transport and
// each good frame, it says what to do. LtcChaseDriver and LtcGenerator are
// declared here and defined in src/ltc_driver.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "sonora/api/engine_control.h"
#include "sonora/api/state_block.h"
#include "sonora/ltc/ltc.h"

namespace sonora::ltc {

// One input channel's samples, as the tap delivers them. Returns how many
// it wrote into `out`; 0 means none are waiting.
class LtcSampleSource {
public:
    virtual ~LtcSampleSource() = default;
    virtual std::size_t read(Span<float> out) = 0;
};

// One output channel. Returns how many it took; fewer than `in.size` is a
// short write, which the generator counts and never waits on.
class LtcSampleSink {
public:
    virtual ~LtcSampleSink() = default;
    virtual std::size_t write(Span<const float> in) = 0;
};

enum class LtcMode : std::uint8_t { Off, Chase, Generate };

struct LtcConfig {
    LtcMode mode = LtcMode::Off;
    std::uint32_t sample_rate = 48'000;
    LtcRate rate = LtcRate::Fps25;
    // How far the playhead may sit from the timecode before a locate.
    std::uint32_t window_frames = 2;
    // Control ticks with no good frame before the chase stops the transport.
    // A tick count, never a clock (ADR-0124 §4).
    std::uint32_t silence_ticks = 8;
    // Session sample = timecode sample + offset. A target below zero is
    // before the session and is not chased.
    std::int64_t offset_samples = 0;
};

// Start-up validation (ADR-0124 §5): false, with the reason in `why`, for a
// configuration that must not run. The caller shows `why` to the operator.
inline bool validate_ltc_config(const LtcConfig& c, LtcMode wanted, LtcRate session_rate,
                                std::string& why) {
    if (c.mode != wanted) {
        why = "LTC: the configured mode is not the one this driver runs";
        return false;
    }
    if (c.rate != session_rate) {
        why = "LTC: the timecode rate differs from the session's; refused";
        return false;
    }
    if (c.sample_rate < 8'000 || c.sample_rate > 384'000) {
        why = "LTC: sample rate out of range";
        return false;
    }
    if (c.mode == LtcMode::Chase && (c.window_frames == 0 || c.silence_ticks == 0)) {
        why = "LTC: the chase window and the silence budget must be at least 1";
        return false;
    }
    return true;
}

enum class ChaseAction : std::uint8_t { None, Locate, Play, Stop };

// The chase policy, the same as MTC's (ADR-0124 §4). Per control tick the
// driver calls observe() with the published transport, on_time() for each
// good frame in signal order, then on_tick() once. Lock needs two frames in
// succession, so one stray frame moves nothing. While recording it never
// locates: a frame outside the window is counted as drift instead, because
// a take is never lost. Allocation-free.
class LtcChase {
public:
    explicit LtcChase(const LtcConfig& config)
        : sample_rate_(config.sample_rate),
          rate_(config.rate),
          silence_budget_(config.silence_ticks),
          offset_(config.offset_samples),
          window_(window_for(config)) {}

    void observe(std::uint64_t playhead, api::TransportState state) {
        playhead_ = playhead;
        state_ = state;
    }

    ChaseAction on_time(const LtcTime& t) {
        if (t.rate != rate_ || !t.valid()) {
            ++rejected_;
            return ChaseAction::None;
        }
        heard_ = true;
        silent_ticks_ = 0;
        const std::uint64_t frames = t.to_frames();
        const bool in_step = have_last_ && frames == (last_ + 1u) % frames_per_day(rate_);
        last_ = frames;
        have_last_ = true;
        locked_ = in_step;
        if (!locked_) return ChaseAction::None;

        // The frame was decoded when its sync word ended, so the timecode
        // now stands at the start of the next frame.
        const LtcTime now = LtcTime::from_frames(frames + 1u, rate_);
        const std::int64_t at = static_cast<std::int64_t>(now.to_samples(sample_rate_)) + offset_;
        if (at < 0) return ChaseAction::None;
        target_ = static_cast<std::uint64_t>(at);
        const std::uint64_t gap = playhead_ > target_ ? playhead_ - target_ : target_ - playhead_;
        const bool inside = gap <= window_;

        switch (state_) {
        case api::TransportState::Recording:
            if (!inside) ++drift_;
            return ChaseAction::None;
        case api::TransportState::Rolling:
            if (inside) return ChaseAction::None;
            playhead_ = target_;   // assume the command lands until the next observe()
            return ChaseAction::Locate;
        case api::TransportState::Stopped:
            if (!inside) {
                playhead_ = target_;
                return ChaseAction::Locate;
            }
            state_ = api::TransportState::Rolling;
            return ChaseAction::Play;
        }
        return ChaseAction::None;
    }

    ChaseAction on_tick() {
        if (heard_) {
            heard_ = false;
            return ChaseAction::None;
        }
        if (!have_last_) return ChaseAction::None;
        if (++silent_ticks_ < silence_budget_) return ChaseAction::None;
        have_last_ = false;
        locked_ = false;
        silent_ticks_ = 0;
        if (state_ == api::TransportState::Stopped) return ChaseAction::None;
        state_ = api::TransportState::Stopped;
        return ChaseAction::Stop;
    }

    void reset() {
        playhead_ = 0;
        state_ = api::TransportState::Stopped;
        target_ = 0;
        last_ = 0;
        silent_ticks_ = 0;
        drift_ = 0;
        rejected_ = 0;
        have_last_ = false;
        locked_ = false;
        heard_ = false;
    }

    // The session sample a Locate goes to.
    std::uint64_t target() const { return target_; }
    std::uint64_t window_samples() const { return window_; }
    bool locked() const { return locked_; }
    // Frames outside the window while recording, left alone.
    std::uint64_t drift() const { return drift_; }
    // Frames of another rate or out of range.
    std::uint64_t rejected() const { return rejected_; }

private:
    // The window in samples, rounded up so a 2-frame window always admits
    // a playhead exactly 2 frames away.
    static std::uint64_t window_for(const LtcConfig& c) {
        const RateRatio r = rate_ratio(c.rate);
        const std::uint64_t scaled = std::uint64_t{c.window_frames} * c.sample_rate * r.den;
        return (scaled + r.num - 1u) / r.num;
    }

    std::uint32_t sample_rate_;
    LtcRate rate_;
    std::uint32_t silence_budget_;
    std::int64_t offset_;
    std::uint64_t window_;
    std::uint64_t playhead_ = 0;
    api::TransportState state_ = api::TransportState::Stopped;
    std::uint64_t target_ = 0;
    std::uint64_t last_ = 0;
    std::uint32_t silent_ticks_ = 0;
    std::uint64_t drift_ = 0;
    std::uint64_t rejected_ = 0;
    bool have_last_ = false;
    bool locked_ = false;
    bool heard_ = false;
};

// Follows incoming LTC: each tick drains the source through the decoder,
// runs LtcChase and sends transport.locate / transport.play /
// transport.stop. Does nothing until start() accepted the configuration.
class LtcChaseDriver {
public:
    static constexpr std::size_t kChunk = 1024;
    // Bounds one tick's work when the source runs ahead.
    static constexpr std::size_t kMaxChunksPerTick = 64;

    LtcChaseDriver(api::EngineControl& engine, LtcSampleSource& source, const LtcConfig& config);

    bool start(LtcRate session_rate, std::string& why);
    void tick();

    const LtcChase& chase() const { return chase_; }
    std::uint64_t commands_sent() const { return commands_sent_; }

private:
    void act(ChaseAction action);

    api::EngineControl& engine_;
    LtcSampleSource& source_;
    LtcConfig config_;
    LtcDecoder decoder_;
    LtcChase chase_;
    LtcDecoded decoded_;
    float scratch_[kChunk] = {};
    std::uint64_t commands_sent_ = 0;
    bool started_ = false;
};

// Emits LTC for the transport: each tick, while rolling, renders the
// playhead's advance since the previous tick and writes it to the sink; a
// jump in the playhead restarts the timecode there. Stopped, it writes
// nothing. Does nothing until start() accepted the configuration.
class LtcGenerator {
public:
    static constexpr std::size_t kChunk = 1024;

    LtcGenerator(api::EngineControl& engine, LtcSampleSink& sink, const LtcConfig& config);

    bool start(LtcRate session_rate, std::string& why);
    void tick();

    std::uint64_t short_writes() const { return short_writes_; }

private:
    api::EngineControl& engine_;
    LtcSampleSink& sink_;
    LtcConfig config_;
    LtcEncoder encoder_;
    LtcTime next_;
    std::uint64_t last_playhead_ = 0;
    float scratch_[kChunk] = {};
    std::uint64_t short_writes_ = 0;
    bool rolling_ = false;
    bool started_ = false;
};

}  // namespace sonora::ltc
