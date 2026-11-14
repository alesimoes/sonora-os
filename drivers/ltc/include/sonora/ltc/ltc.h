// SPDX-License-Identifier: GPL-3.0-or-later
// LTC (SMPTE 12M linear timecode) time maths and codec (ADR-0124).
//
// LtcTime and its frame/sample conversions are inline and pure. The codec
// classes are declared here and defined in src/ltc_codec.cpp. Neither the
// decoder nor the encoder allocates after construction, so both may later
// run on the audio thread (ADR-0124 §2, §3); today they run on the control
// thread only.
#pragma once

#include <cstddef>
#include <cstdint>

namespace sonora::ltc {

// A non-owning view of samples. C++17 has no std::span; this is the part of
// it the codec needs.
template <class T>
struct Span {
    T* data = nullptr;
    std::size_t size = 0;

    T& operator[](std::size_t i) const { return data[i]; }
    T* begin() const { return data; }
    T* end() const { return data + size; }
};

enum class LtcRate : std::uint8_t { Fps24, Fps25, Fps2997Drop, Fps30 };

// Frames per second as a ratio: 29.97 drop frame runs 30000 frames every
// 1001 s, so sample positions come out exact in integers.
struct RateRatio {
    std::uint64_t num;
    std::uint64_t den;
};

inline constexpr RateRatio rate_ratio(LtcRate rate) {
    switch (rate) {
    case LtcRate::Fps24: return {24, 1};
    case LtcRate::Fps25: return {25, 1};
    case LtcRate::Fps2997Drop: return {30000, 1001};
    case LtcRate::Fps30: return {30, 1};
    }
    return {30, 1};
}

// The frame numbers a label counts through per second (30 for drop frame:
// drop frame skips labels, not time).
inline constexpr std::uint32_t nominal_fps(LtcRate rate) {
    switch (rate) {
    case LtcRate::Fps24: return 24;
    case LtcRate::Fps25: return 25;
    case LtcRate::Fps2997Drop:
    case LtcRate::Fps30: return 30;
    }
    return 30;
}

// Frames in 24 hours, where timecode wraps. Drop frame loses 2 labels in
// 54 of every 60 minutes: 108 000 * 24 - 2 * 54 * 24.
inline constexpr std::uint64_t frames_per_day(LtcRate rate) {
    return rate == LtcRate::Fps2997Drop ? 2'589'408u
                                        : std::uint64_t{nominal_fps(rate)} * 86'400u;
}

struct LtcTime {
    std::uint8_t h = 0, m = 0, s = 0, frame = 0;
    LtcRate rate = LtcRate::Fps25;

    // In range for its rate, and not a label drop frame skips (frames 0 and
    // 1 at second 0 of a minute not divisible by ten).
    constexpr bool valid() const {
        if (h > 23 || m > 59 || s > 59 || frame >= nominal_fps(rate)) return false;
        return !(rate == LtcRate::Fps2997Drop && s == 0 && frame < 2 && m % 10 != 0);
    }

    // Frames since 00:00:00:00 of the same day.
    constexpr std::uint64_t to_frames() const {
        const std::uint64_t fps = nominal_fps(rate);
        const std::uint64_t minutes = std::uint64_t{h} * 60u + m;
        const std::uint64_t labels = (minutes * 60u + s) * fps + frame;
        if (rate != LtcRate::Fps2997Drop) return labels;
        return labels - 2u * (minutes - minutes / 10u);
    }

    // The label of frame `n`, wrapped at 24 hours.
    static constexpr LtcTime from_frames(std::uint64_t n, LtcRate rate) {
        n %= frames_per_day(rate);
        if (rate == LtcRate::Fps2997Drop) {
            // Put back the labels drop frame skipped, then count as 30 fps.
            constexpr std::uint64_t kPerTen = 17'982;   // frames in 10 minutes
            constexpr std::uint64_t kPerMinute = 1'798; // minutes 1..9 of ten
            const std::uint64_t tens = n / kPerTen;
            const std::uint64_t rem = n % kPerTen;
            n += 18u * tens + (rem > 1 ? 2u * ((rem - 2u) / kPerMinute) : 0u);
        }
        const std::uint64_t fps = nominal_fps(rate);
        LtcTime t;
        t.rate = rate;
        t.frame = static_cast<std::uint8_t>(n % fps);
        n /= fps;
        t.s = static_cast<std::uint8_t>(n % 60u);
        n /= 60u;
        t.m = static_cast<std::uint8_t>(n % 60u);
        t.h = static_cast<std::uint8_t>(n / 60u);
        return t;
    }

    // The first sample at or after this frame's start. Rounding up is what
    // makes from_samples(to_samples(t)) == t at every rate down to 1 sample
    // per frame.
    constexpr std::uint64_t to_samples(std::uint32_t sample_rate) const {
        const RateRatio r = rate_ratio(rate);
        const std::uint64_t scaled = to_frames() * sample_rate * r.den;
        return (scaled + r.num - 1u) / r.num;
    }

    // The frame that sample `sample` falls in, wrapped at 24 hours.
    static constexpr LtcTime from_samples(std::uint64_t sample, std::uint32_t sample_rate,
                                          LtcRate rate) {
        const RateRatio r = rate_ratio(rate);
        return from_frames(sample * r.num / (std::uint64_t{sample_rate} * r.den), rate);
    }

    constexpr bool operator==(const LtcTime& o) const {
        return h == o.h && m == o.m && s == o.s && frame == o.frame && rate == o.rate;
    }
    constexpr bool operator!=(const LtcTime& o) const { return !(*this == o); }
};

// The good frames one feed() found, in signal order. Fixed size so a feed
// never allocates; a block long enough to hold more counts them in
// `overflow` instead (callers feed blocks of a few frames).
struct LtcDecoded {
    static constexpr std::size_t kMaxFrames = 16;
    LtcTime time[kMaxFrames];
    // Index in the fed block of the sample just after the frame's sync word.
    std::size_t end_offset[kMaxFrames] = {};
    std::size_t count = 0;
    std::uint32_t overflow = 0;
};

// Biphase-mark decoder (ADR-0124 §2): zero-crossing intervals against a
// self-adjusting bit clock, a search for the sync word 0x3FFD, one LtcTime
// per frame whose fields are in range for `rate`. A reversed sync word
// (tape running backwards) yields nothing in v1. State survives across
// feed() calls, so block boundaries are free.
class LtcDecoder {
public:
    LtcDecoder(std::uint32_t sample_rate, LtcRate rate);

    // Clears `out`, then appends every good frame completed inside `in`.
    void feed(Span<const float> in, LtcDecoded& out);
    // Forget the bit clock and the partial frame, as at construction.
    void reset();

    std::uint64_t good_frames() const { return good_frames_; }
    LtcRate rate() const { return rate_; }

private:
    std::uint32_t sample_rate_;
    LtcRate rate_;
    float bit_period_ = 0.0f;      // samples per bit, tracked
    float since_crossing_ = 0.0f;  // samples since the last zero crossing
    float envelope_ = 0.0f;        // peak level, for the hysteresis threshold
    bool high_ = false;            // which side of the threshold the signal is
    bool half_pending_ = false;    // one short interval of a '1' seen
    std::uint64_t bits_lo_ = 0;    // the 80 newest bits: oldest 64 here
    std::uint16_t bits_hi_ = 0;    // and the newest 16, where the sync word lands
    std::uint32_t bits_seen_ = 0;  // since reset, saturating at 80
    std::uint64_t good_frames_ = 0;
};

// Biphase-mark encoder (ADR-0124 §3). The waveform's level and the position
// inside the frame carry across render() calls, so a frame may span blocks.
class LtcEncoder {
public:
    LtcEncoder(std::uint32_t sample_rate, LtcRate rate, float amplitude = 0.5f);

    // Fills all of `out`. A frame part-written by the previous call is
    // finished first; each frame started in this call is `next`, then
    // next + 1, and so on. Returns what the following call should pass to
    // continue without a jump (the frame after the last one started).
    LtcTime render(const LtcTime& next, Span<float> out);
    // Silence the partial frame and restart the bit clock at a frame edge.
    void reset();

private:
    std::uint32_t sample_rate_;
    LtcRate rate_;
    float amplitude_;
    float level_ = 1.0f;            // the biphase level, flipped at each edge
    std::uint64_t samples_ = 0;     // rendered since reset
    std::uint64_t half_bits_ = 0;   // half-bit cells started since reset
    std::uint64_t frame_lo_ = 0;    // the frame being written: bits 0..63
    std::uint16_t frame_hi_ = 0;    // and bits 64..79 (the sync word)
    bool frame_open_ = false;
};

}  // namespace sonora::ltc
