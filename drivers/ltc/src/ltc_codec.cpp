// SPDX-License-Identifier: GPL-3.0-or-later
// LTC biphase-mark decoder and encoder (ADR-0124 §2, §3).
//
// Frame layout (SMPTE 12M, bits in transmission order, BCD least
// significant bit first): frame units 0-3, frame tens 8-9, drop frame 10,
// seconds 16-19 / 24-26, minutes 32-35 / 40-42, hours 48-51 / 56-57, the
// sync word in 64-79. User bits and the binary group flags are written as
// zero and ignored on input.
//
// Neither class allocates: all state is the fixed members in ltc.h.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "sonora/ltc/ltc.h"

namespace sonora::ltc {
namespace {

// Bits 64..79 read in transmission order, bit 64 as the most significant.
constexpr std::uint16_t kSyncWord = 0x3FFD;
constexpr std::uint32_t kFrameBits = 80;
constexpr std::uint32_t kHalfBitsPerFrame = 2 * kFrameBits;

// An interval below this share of a bit is a half bit (one half of a '1');
// above it, a whole bit (a '0'). Halfway between 0.5 and 1 gives the most
// room either side for jitter and varispeed.
constexpr float kHalfOrWhole = 0.75f;
// Shorter or longer than these is not biphase mark: noise, a dropout, or
// the first crossing after silence. Both leave room for +-10 % varispeed.
constexpr float kTooShort = 0.3f;
constexpr float kTooLong = 1.4f;
// How fast the bit clock follows the signal, per interval.
constexpr float kClockGain = 0.1f;
// The bit clock never wanders further than this from the nominal rate.
constexpr float kClockMin = 0.75f;
constexpr float kClockMax = 1.33f;
// Crossings count beyond this share of the signal's peak, so ringing and
// noise around zero do not.
constexpr float kHysteresis = 0.25f;
// Below this level (about -50 dBFS) nothing counts as signal. The usable
// minimum from a real interface is unmeasured (ADR-0124, Unverified).
constexpr float kFloor = 0.003f;
// The peak follower's release, in seconds.
constexpr float kEnvelopeRelease = 0.005f;
// since_crossing_ stops counting here, so silence never loses float
// precision; anything this long is "no signal" anyway.
constexpr float kIdle = 1.0e6f;

// The bit that carries the biphase polarity correction: 59 at 25 fps, 27
// at the others.
std::uint32_t polarity_bit(LtcRate rate) { return rate == LtcRate::Fps25 ? 59u : 27u; }

std::uint64_t field(std::uint64_t bits, std::uint32_t pos, std::uint32_t width) {
    return (bits >> pos) & ((std::uint64_t{1} << width) - 1u);
}

// Bits 0..63 of the frame for `t`, labelled as `rate`.
std::uint64_t pack_frame(const LtcTime& t, LtcRate rate) {
    std::uint64_t bits = 0;
    const auto put = [&bits](std::uint32_t pos, std::uint64_t value) { bits |= value << pos; };
    put(0, t.frame % 10u);
    put(8, t.frame / 10u);
    put(10, rate == LtcRate::Fps2997Drop ? 1u : 0u);
    put(16, t.s % 10u);
    put(24, t.s / 10u);
    put(32, t.m % 10u);
    put(40, t.m / 10u);
    put(48, t.h % 10u);
    put(56, t.h / 10u);
    // An even count of ones over all 80 bits makes every frame start on the
    // same level, which receivers that check polarity expect.
    std::uint32_t ones = 0;
    for (std::uint64_t v = bits; v != 0; v &= v - 1u) ++ones;
    for (std::uint32_t v = kSyncWord; v != 0; v &= v - 1u) ++ones;
    if (ones % 2u != 0) put(polarity_bit(rate), 1u);
    return bits;
}

// The time in bits 0..63, if every digit is BCD, the drop frame flag
// agrees with `rate` and the label exists at that rate.
bool parse_frame(std::uint64_t bits, LtcRate rate, LtcTime& t) {
    const std::uint64_t fu = field(bits, 0, 4), su = field(bits, 16, 4);
    const std::uint64_t mu = field(bits, 32, 4), hu = field(bits, 48, 4);
    if (fu > 9 || su > 9 || mu > 9 || hu > 9) return false;
    const bool drop = field(bits, 10, 1) != 0;
    if (drop != (rate == LtcRate::Fps2997Drop)) return false;
    t.rate = rate;
    t.frame = static_cast<std::uint8_t>(field(bits, 8, 2) * 10u + fu);
    t.s = static_cast<std::uint8_t>(field(bits, 24, 3) * 10u + su);
    t.m = static_cast<std::uint8_t>(field(bits, 40, 3) * 10u + mu);
    t.h = static_cast<std::uint8_t>(field(bits, 56, 2) * 10u + hu);
    return t.valid();
}

// Samples per bit at the nominal rate.
float nominal_bit_period(std::uint32_t sample_rate, LtcRate rate) {
    const RateRatio r = rate_ratio(rate);
    return static_cast<float>(static_cast<double>(sample_rate) * static_cast<double>(r.den) /
                              (static_cast<double>(r.num) * kFrameBits));
}

}  // namespace

// ---------------------------------------------------------------- decoder

LtcDecoder::LtcDecoder(std::uint32_t sample_rate, LtcRate rate)
    : sample_rate_(sample_rate), rate_(rate) {
    reset();
}

void LtcDecoder::reset() {
    bit_period_ = nominal_bit_period(sample_rate_, rate_);
    // As if the last crossing was long ago: the first one is not an interval.
    since_crossing_ = kIdle;
    envelope_ = 0.0f;
    high_ = false;
    half_pending_ = false;
    bits_lo_ = 0;
    bits_hi_ = 0;
    bits_seen_ = 0;
    good_frames_ = 0;
}

void LtcDecoder::feed(Span<const float> in, LtcDecoded& out) {
    out.count = 0;
    out.overflow = 0;
    const float nominal = nominal_bit_period(sample_rate_, rate_);
    const float release = 1.0f - 1.0f / (kEnvelopeRelease * static_cast<float>(sample_rate_));

    // Shift one decoded bit in; on the sync word after a whole frame, emit.
    const auto push_bit = [&](std::uint32_t bit, std::size_t at) {
        const std::uint64_t leaving = (bits_hi_ >> 15) & 1u;
        bits_hi_ = static_cast<std::uint16_t>((bits_hi_ << 1) | bit);
        bits_lo_ = (bits_lo_ >> 1) | (leaving << 63);
        if (bits_seen_ < kFrameBits) ++bits_seen_;
        // A reversed sync word (0xBFFC, tape backwards) is not matched: v1
        // reports it as no lock (ADR-0124 §2).
        if (bits_hi_ != kSyncWord) return;
        const bool whole = bits_seen_ >= kFrameBits;
        bits_seen_ = 0;
        LtcTime t;
        if (!whole || !parse_frame(bits_lo_, rate_, t)) return;
        ++good_frames_;
        if (out.count == LtcDecoded::kMaxFrames) {
            ++out.overflow;
            return;
        }
        out.time[out.count] = t;
        out.end_offset[out.count] = at;
        ++out.count;
    };

    // A glitch drops the partial frame; the bit clock is kept.
    const auto lose_frame = [&]() {
        half_pending_ = false;
        bits_seen_ = 0;
    };

    const auto on_interval = [&](float interval, std::size_t at) {
        if (interval < kTooShort * bit_period_ || interval > kTooLong * bit_period_) {
            lose_frame();
            return;
        }
        float measured;
        if (interval < kHalfOrWhole * bit_period_) {
            measured = 2.0f * interval;
            if (!half_pending_) {
                half_pending_ = true;
            } else {
                half_pending_ = false;
                push_bit(1u, at);
            }
        } else {
            measured = interval;
            if (half_pending_) {
                // A lone half bit: the halves were paired wrongly.
                lose_frame();
            } else {
                push_bit(0u, at);
            }
        }
        bit_period_ += kClockGain * (measured - bit_period_);
        bit_period_ = std::min(std::max(bit_period_, kClockMin * nominal), kClockMax * nominal);
    };

    for (std::size_t i = 0; i < in.size; ++i) {
        const float x = in[i];
        envelope_ = std::max(std::fabs(x), envelope_ * release);
        if (since_crossing_ < kIdle) since_crossing_ += 1.0f;
        const float threshold = std::max(envelope_ * kHysteresis, kFloor);
        const bool crossed = high_ ? x < -threshold : x > threshold;
        if (!crossed) continue;
        high_ = !high_;
        const float interval = since_crossing_;
        since_crossing_ = 0.0f;
        on_interval(interval, i);
    }
}

// ---------------------------------------------------------------- encoder

LtcEncoder::LtcEncoder(std::uint32_t sample_rate, LtcRate rate, float amplitude)
    : sample_rate_(sample_rate), rate_(rate), amplitude_(amplitude) {
    reset();
}

void LtcEncoder::reset() {
    samples_ = 0;
    half_bits_ = 0;
    frame_lo_ = 0;
    frame_hi_ = 0;
    frame_open_ = false;
}

LtcTime LtcEncoder::render(const LtcTime& next, Span<float> out) {
    const RateRatio r = rate_ratio(rate_);
    const std::uint64_t cells_per_second_num = r.num * kHalfBitsPerFrame;
    // The first sample of half-bit cell k, rounded up, exact in integers so
    // the cells never drift from the sample clock however long it runs.
    const auto cell_start = [&](std::uint64_t k) {
        const std::uint64_t scaled = k * sample_rate_ * r.den;
        return (scaled + cells_per_second_num - 1u) / cells_per_second_num;
    };

    LtcTime cur = next;
    const auto begin_cell = [&]() {
        const std::uint64_t cell = half_bits_ % kHalfBitsPerFrame;
        if (cell == 0) {
            frame_lo_ = pack_frame(cur, rate_);
            frame_hi_ = kSyncWord;
            frame_open_ = true;
            cur = LtcTime::from_frames(cur.to_frames() + 1u, rate_);
        }
        const std::uint64_t bit = cell / 2u;
        const std::uint64_t value =
            bit < 64u ? (frame_lo_ >> bit) & 1u : (frame_hi_ >> (kFrameBits - 1u - bit)) & 1u;
        // Biphase mark: an edge at every bit start, and one mid-bit for a '1'.
        if (cell % 2u == 0 || value != 0) level_ = -level_;
        ++half_bits_;
        if (half_bits_ % kHalfBitsPerFrame == 0) frame_open_ = false;
    };

    for (std::size_t i = 0; i < out.size; ++i) {
        while (cell_start(half_bits_) <= samples_) begin_cell();
        out[i] = level_ * amplitude_;
        ++samples_;
    }
    return cur;
}

}  // namespace sonora::ltc
