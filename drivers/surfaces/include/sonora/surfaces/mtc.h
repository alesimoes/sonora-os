// SPDX-License-Identifier: GPL-3.0-or-later
// MIDI Time Code (ADR-0122): the timecode arithmetic, the quarter-frame and
// full-frame codec, and the chase rule that turns a master's position into
// transport commands. Nothing here runs on the audio thread: the codec is
// allocation-free, but it belongs to the control thread that polls the
// MidiPort, as the MCU and HUI protocols do. The chase only ever reaches the
// engine through api::EngineControl, with the commands the UI already sends.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "sonora/api/engine_control.h"
#include "sonora/surfaces/midi_port.h"

namespace sonora::surfaces {

// The values are the MTC rate code (quarter-frame piece 7, bits 1-2, and the
// top bits of the full-frame hour byte), so the codec casts, never maps.
enum class FrameRate : std::uint8_t { Fps24 = 0, Fps25 = 1, Fps2997Drop = 2, Fps30 = 3 };

struct Timecode {
    std::uint8_t hours = 0;
    std::uint8_t minutes = 0;
    std::uint8_t seconds = 0;
    std::uint8_t frames = 0;
    FrameRate rate = FrameRate::Fps25;
};

inline bool operator==(const Timecode& a, const Timecode& b) {
    return a.hours == b.hours && a.minutes == b.minutes && a.seconds == b.seconds &&
           a.frames == b.frames && a.rate == b.rate;
}
inline bool operator!=(const Timecode& a, const Timecode& b) { return !(a == b); }

// Frames labelled per second: 29.97 drop-frame counts 30 labels and skips
// some of them, so its label space is 30 wide.
constexpr std::uint32_t nominal_fps(FrameRate r) {
    return r == FrameRate::Fps24 ? 24u : r == FrameRate::Fps25 ? 25u : 30u;
}

// Real frames per second as num/den; only drop-frame is not an integer.
struct RateRatio {
    std::uint64_t num;
    std::uint64_t den;
};
constexpr RateRatio rate_ratio(FrameRate r) {
    return r == FrameRate::Fps2997Drop ? RateRatio{30000u, 1001u} : RateRatio{nominal_fps(r), 1u};
}

namespace mtc_detail {
constexpr std::int64_t kDropFramesPer10Min = 17982;  // 10 * 1800 - 9 * 2
constexpr std::int64_t kDropFramesPerMin = 1798;     // a minute that drops 2
}  // namespace mtc_detail

constexpr std::int64_t frames_per_day(FrameRate r) {
    return r == FrameRate::Fps2997Drop ? 24 * 6 * mtc_detail::kDropFramesPer10Min
                                       : std::int64_t{24} * 3600 * nominal_fps(r);
}

// In range for its rate. Drop-frame has no labels ;00 and ;01 at the start
// of a minute not divisible by ten.
inline bool valid(const Timecode& tc) {
    if (tc.hours > 23 || tc.minutes > 59 || tc.seconds > 59) return false;
    if (tc.frames >= nominal_fps(tc.rate)) return false;
    if (tc.rate == FrameRate::Fps2997Drop && tc.seconds == 0 && tc.frames < 2 &&
        tc.minutes % 10 != 0) {
        return false;
    }
    return true;
}

// Frames elapsed since 00:00:00:00 of the same day. An invalid drop-frame
// label is counted as if it existed; callers check valid() first.
inline std::int64_t to_frames(const Timecode& tc) {
    const std::int64_t fps = nominal_fps(tc.rate);
    const std::int64_t total_minutes = std::int64_t{60} * tc.hours + tc.minutes;
    std::int64_t n = (total_minutes * 60 + tc.seconds) * fps + tc.frames;
    if (tc.rate == FrameRate::Fps2997Drop) n -= 2 * (total_minutes - total_minutes / 10);
    return n;
}

// The inverse; `frames` wraps modulo one day either way, as MTC does.
inline Timecode from_frames(std::int64_t frames, FrameRate rate) {
    const std::int64_t day = frames_per_day(rate);
    std::int64_t n = frames % day;
    if (n < 0) n += day;
    if (rate == FrameRate::Fps2997Drop) {
        using namespace mtc_detail;
        const std::int64_t d = n / kDropFramesPer10Min;
        const std::int64_t m = n % kDropFramesPer10Min;
        n += 18 * d + (m > 1 ? 2 * ((m - 2) / kDropFramesPerMin) : 0);
    }
    const std::int64_t fps = nominal_fps(rate);
    Timecode tc;
    tc.rate = rate;
    tc.frames = static_cast<std::uint8_t>(n % fps);
    tc.seconds = static_cast<std::uint8_t>((n / fps) % 60);
    tc.minutes = static_cast<std::uint8_t>((n / (fps * 60)) % 60);
    tc.hours = static_cast<std::uint8_t>(n / (fps * 3600));
    return tc;
}

inline Timecode add_frames(const Timecode& tc, std::int64_t n) {
    return from_frames(to_frames(tc) + n, tc.rate);
}

// The first sample at or after the frame's start (ceil), so from_samples of
// the result gives the frame back at any sample rate.
inline std::uint64_t frames_to_samples(std::int64_t frames, FrameRate rate,
                                       std::uint32_t sample_rate) {
    const RateRatio r = rate_ratio(rate);
    const std::uint64_t f = frames < 0 ? 0u : static_cast<std::uint64_t>(frames);
    return (f * sample_rate * r.den + r.num - 1) / r.num;
}

inline std::uint64_t to_samples(const Timecode& tc, std::uint32_t sample_rate) {
    return frames_to_samples(to_frames(tc), tc.rate, sample_rate);
}

// The frame a sample falls in (floor), wrapped to one day.
inline Timecode from_samples(std::uint64_t samples, FrameRate rate, std::uint32_t sample_rate) {
    const RateRatio r = rate_ratio(rate);
    const std::uint64_t frames = samples * r.num / (std::uint64_t{sample_rate} * r.den);
    return from_frames(static_cast<std::int64_t>(frames % static_cast<std::uint64_t>(
                           frames_per_day(rate))),
                       rate);
}

// --- Codec -----------------------------------------------------------------

constexpr std::uint8_t kMtcQuarterFrame = 0xF1;
constexpr std::size_t kMtcFullFrameBytes = 10;

// Generation. A cycle of 8 quarter-frames (F1 0p dddd) carries one value and
// takes two frames to send; the next cycle carries the value two frames on.
// Pacing (one piece every quarter frame) is the caller's: it calls next()
// from whatever clock it runs on.
class MtcEncoder {
public:
    // Piece 0..7 of `tc`, as the two bytes F1 nn.
    static void quarter_frame(const Timecode& tc, unsigned piece, std::uint8_t out[2]) {
        const std::uint8_t rate = static_cast<std::uint8_t>(tc.rate);
        std::uint8_t nibble = 0;
        switch (piece & 7u) {
            case 0: nibble = tc.frames & 0x0F; break;
            case 1: nibble = (tc.frames >> 4) & 0x01; break;
            case 2: nibble = tc.seconds & 0x0F; break;
            case 3: nibble = (tc.seconds >> 4) & 0x03; break;
            case 4: nibble = tc.minutes & 0x0F; break;
            case 5: nibble = (tc.minutes >> 4) & 0x03; break;
            case 6: nibble = tc.hours & 0x0F; break;
            default: nibble = static_cast<std::uint8_t>(((tc.hours >> 4) & 0x01) | (rate << 1)); break;
        }
        out[0] = kMtcQuarterFrame;
        out[1] = static_cast<std::uint8_t>(((piece & 7u) << 4) | nibble);
    }

    // All 8 pieces of one cycle, back to back (16 bytes).
    static void cycle(const Timecode& tc, std::uint8_t out[16]) {
        for (unsigned p = 0; p < 8; ++p) quarter_frame(tc, p, out + 2 * p);
    }

    // F0 7F 7F 01 01 hr mn sc fr F7: a locate, sent when the master jumps or
    // stops; receivers take it at once.
    static void full_frame(const Timecode& tc, std::uint8_t out[kMtcFullFrameBytes]) {
        out[0] = 0xF0;
        out[1] = 0x7F;  // real-time universal
        out[2] = 0x7F;  // all-call device
        out[3] = 0x01;  // MTC
        out[4] = 0x01;  // full message
        out[5] = static_cast<std::uint8_t>((static_cast<std::uint8_t>(tc.rate) << 5) | (tc.hours & 0x1F));
        out[6] = tc.minutes;
        out[7] = tc.seconds;
        out[8] = tc.frames;
        out[9] = 0xF7;
    }

    // Running generation from `tc`, piece 0 first.
    void start(const Timecode& tc) {
        position_ = tc;
        piece_ = 0;
    }
    // The next piece; after piece 7 the carried value moves two frames on.
    void next(std::uint8_t out[2]) {
        quarter_frame(position_, piece_, out);
        if (++piece_ == 8) {
            piece_ = 0;
            position_ = add_frames(position_, 2);
        }
    }
    const Timecode& position() const { return position_; }

    bool send_next(MidiPort& port) {
        std::uint8_t b[2];
        next(b);
        return port.write(b, 2);
    }
    static bool send_full_frame(MidiPort& port, const Timecode& tc) {
        std::uint8_t b[kMtcFullFrameBytes];
        full_frame(tc, b);
        return port.write(b, kMtcFullFrameBytes);
    }

private:
    Timecode position_{};
    unsigned piece_ = 0;
};

struct MtcEvent {
    enum class Kind : std::uint8_t {
        QuarterFrame,  // a complete cycle while locked: the master rolls
        FullFrame,     // a locate: take it at once
    };
    Kind kind = Kind::QuarterFrame;
    Timecode position{};
};

// Reception. Bytes arrive in chunks that may split any message; the parser
// keeps its state between calls. A quarter-frame position is reported once
// the 8 pieces 0..7 arrived consecutively and in order, and again at the end
// of every further cycle. It is the carried value plus the two frames the
// cycle took to send, i.e. where the master is when piece 7 arrives. A piece
// out of sequence drops the lock; it comes back after a fresh 0..7. Reverse
// play (pieces 7..0) is not chased and reads as a gap. Real-time bytes
// (F8..FF) may interleave anywhere and are skipped.
class MtcDecoder {
public:
    // One byte; true when it completes an event.
    bool push(std::uint8_t b, MtcEvent& out) {
        if (b >= 0xF8) return false;
        if (b & 0x80) return status(b, out);
        switch (state_) {
            case State::QuarterData:
                state_ = State::Idle;
                return quarter(b, out);
            case State::SysEx:
                if (sysex_len_ < sizeof(sysex_)) sysex_[sysex_len_] = b;
                ++sysex_len_;
                return false;
            case State::Idle:
                return false;
        }
        return false;
    }

    template <class OnEvent>
    void feed(const std::uint8_t* bytes, std::size_t n, OnEvent&& on_event) {
        MtcEvent e;
        for (std::size_t i = 0; i < n; ++i) {
            if (push(bytes[i], e)) on_event(e);
        }
    }

    // Drains the port; returns how many bytes were read.
    template <class OnEvent>
    std::size_t poll(MidiPort& port, OnEvent&& on_event) {
        std::uint8_t buf[64];
        std::size_t total = 0;
        for (;;) {
            const std::size_t n = port.read(buf, sizeof(buf));
            if (n == 0) return total;
            feed(buf, n, on_event);
            total += n;
        }
    }

    bool locked() const { return locked_; }
    // Forget the cycle in progress and the lock (the master went silent).
    void reset() {
        run_ = 0;
        locked_ = false;
    }

private:
    enum class State : std::uint8_t { Idle, QuarterData, SysEx };

    bool status(std::uint8_t b, MtcEvent& out) {
        if (b == 0xF7) {
            const bool was = state_ == State::SysEx;
            state_ = State::Idle;
            return was && full(out);
        }
        state_ = b == kMtcQuarterFrame ? State::QuarterData
                 : b == 0xF0           ? State::SysEx
                                       : State::Idle;
        sysex_len_ = 0;
        return false;
    }

    bool quarter(std::uint8_t d, MtcEvent& out) {
        const unsigned piece = (d >> 4) & 7u;
        if (run_ > 0 && piece != expect_) reset();
        if (run_ == 0 && piece != 0) return false;  // wait for a cycle start
        nibbles_[piece] = d & 0x0F;
        expect_ = (piece + 1) & 7u;
        if (run_ < 8) ++run_;
        if (piece != 7) return false;
        Timecode tc;
        tc.frames = static_cast<std::uint8_t>(nibbles_[0] | ((nibbles_[1] & 0x01) << 4));
        tc.seconds = static_cast<std::uint8_t>(nibbles_[2] | ((nibbles_[3] & 0x03) << 4));
        tc.minutes = static_cast<std::uint8_t>(nibbles_[4] | ((nibbles_[5] & 0x03) << 4));
        tc.hours = static_cast<std::uint8_t>(nibbles_[6] | ((nibbles_[7] & 0x01) << 4));
        tc.rate = static_cast<FrameRate>((nibbles_[7] >> 1) & 0x03);
        if (!valid(tc)) {
            reset();
            return false;
        }
        locked_ = true;
        out.kind = MtcEvent::Kind::QuarterFrame;
        out.position = add_frames(tc, 2);
        return true;
    }

    // sysex_ holds the bytes between F0 and F7: 7F dev 01 01 hr mn sc fr.
    bool full(MtcEvent& out) {
        if (sysex_len_ != 8 || sysex_[0] != 0x7F || sysex_[2] != 0x01 || sysex_[3] != 0x01) {
            return false;
        }
        Timecode tc;
        tc.rate = static_cast<FrameRate>((sysex_[4] >> 5) & 0x03);
        tc.hours = sysex_[4] & 0x1F;
        tc.minutes = sysex_[5];
        tc.seconds = sysex_[6];
        tc.frames = sysex_[7];
        if (!valid(tc)) return false;
        reset();  // the master restarts its cycles from piece 0 after a locate
        out.kind = MtcEvent::Kind::FullFrame;
        out.position = tc;
        return true;
    }

    State state_ = State::Idle;
    std::uint8_t nibbles_[8] = {};
    unsigned expect_ = 0;
    unsigned run_ = 0;
    bool locked_ = false;
    std::uint8_t sysex_[12] = {};
    std::size_t sysex_len_ = 0;
};

// --- Chase -----------------------------------------------------------------

enum class ChaseAction : std::uint8_t { None, Locate, Play, Stop };

// What the chase needs to know of the engine's transport.
struct ChaseSnapshot {
    bool rolling = false;
    std::uint64_t playhead = 0;  // samples
};

// The decision alone, with the engine's transport passed in: no clock, no
// engine, so a test drives it with plain values. Time passes only through
// tick(), counted in frame periods by the caller from a sample or event
// count; never wall-clock.
class ChaseRule {
public:
    static constexpr std::int64_t kLockWindowFrames = 2;
    // A master that stays quiet this long has stopped.
    static constexpr std::uint32_t kSilentFramesToStop = 3;
    // A position this far ahead of the last one is still the master rolling
    // (a quarter-frame cycle advances 2); further is a jump.
    static constexpr std::int64_t kMovingFrames = 4;

    explicit ChaseRule(std::uint32_t sample_rate) : sample_rate_(sample_rate) {}

    ChaseAction on_position(const Timecode& tc, const ChaseSnapshot& engine) {
        silent_ = 0;
        stop_sent_ = false;
        const std::int64_t frames = to_frames(tc);
        const std::int64_t delta = frames - last_frames_;
        const bool moving = have_last_ && delta > 0 && delta <= kMovingFrames;
        have_last_ = true;
        last_frames_ = frames;
        target_ = to_samples(tc, sample_rate_);
        const std::uint64_t window = frames_to_samples(kLockWindowFrames, tc.rate, sample_rate_);
        const std::uint64_t off =
            engine.playhead > target_ ? engine.playhead - target_ : target_ - engine.playhead;
        if (off > window) return ChaseAction::Locate;
        if (moving && !engine.rolling) return ChaseAction::Play;
        return ChaseAction::None;
    }

    // `frame_periods` elapsed with no position.
    ChaseAction tick(std::uint32_t frame_periods, const ChaseSnapshot& engine) {
        if (!have_last_ || stop_sent_) return ChaseAction::None;
        silent_ += frame_periods;
        if (silent_ < kSilentFramesToStop) return ChaseAction::None;
        have_last_ = false;  // the next position starts over, not "moving"
        stop_sent_ = true;
        return engine.rolling ? ChaseAction::Stop : ChaseAction::None;
    }

    // The sample a Locate is to move to.
    std::uint64_t target() const { return target_; }

private:
    std::uint32_t sample_rate_;
    std::uint64_t target_ = 0;
    std::int64_t last_frames_ = 0;
    std::uint32_t silent_ = 0;
    bool have_last_ = false;
    bool stop_sent_ = false;
};

// The rule wired to the engine: reads the published transport, sends the
// same commands the UI does (transport.locate with a sample, transport.play,
// transport.stop). The decoder's owner resets it when this returns Stop.
class MtcChase {
public:
    MtcChase(api::EngineControl& engine, std::uint32_t sample_rate)
        : engine_(engine), rule_(sample_rate) {}

    ChaseAction on_position(const Timecode& tc) { return act(rule_.on_position(tc, snapshot())); }
    ChaseAction tick(std::uint32_t frame_periods) {
        return act(rule_.tick(frame_periods, snapshot()));
    }

private:
    ChaseSnapshot snapshot() {
        const api::TransportSnapshot t = engine_.state().transport();
        return ChaseSnapshot{t.state != api::TransportState::Stopped, t.playhead};
    }
    ChaseAction act(ChaseAction a) {
        api::CommandRequest r;
        switch (a) {
            case ChaseAction::None: return a;
            case ChaseAction::Locate:
                r.command = "transport.locate";
                r.argument = std::to_string(rule_.target());
                break;
            case ChaseAction::Play: r.command = "transport.play"; break;
            case ChaseAction::Stop: r.command = "transport.stop"; break;
        }
        engine_.send(r);
        return a;
    }

    api::EngineControl& engine_;
    ChaseRule rule_;
};

}  // namespace sonora::surfaces
