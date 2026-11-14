// SPDX-License-Identifier: GPL-3.0-or-later
//
// The LTC codec (ADR-0124 §2, §3): what the encoder writes the decoder reads
// back, at every frame rate and three sample rates, however the stream is cut
// into blocks, and nothing at all comes out of silence or noise. Pure
// signal-in, labels-out: no clock, no threads.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "sonora/ltc/ltc.h"

namespace {

int failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            ++failures;                                                  \
        }                                                                \
    } while (0)

using namespace sonora::ltc;

constexpr LtcRate kRates[] = {LtcRate::Fps24, LtcRate::Fps25, LtcRate::Fps2997Drop,
                              LtcRate::Fps30};
constexpr std::uint32_t kSampleRates[] = {44'100, 48'000, 96'000};

LtcTime tc(int h, int m, int s, int f, LtcRate rate) {
    LtcTime t;
    t.h = static_cast<std::uint8_t>(h);
    t.m = static_cast<std::uint8_t>(m);
    t.s = static_cast<std::uint8_t>(s);
    t.frame = static_cast<std::uint8_t>(f);
    t.rate = rate;
    return t;
}

// Samples for `frames` whole frames, rendered in blocks of `block`.
std::vector<float> encode(const LtcTime& start, std::size_t frames, std::uint32_t sample_rate,
                          std::size_t block) {
    const RateRatio r = rate_ratio(start.rate);
    const std::uint64_t total =
        (frames * sample_rate * r.den + r.num - 1u) / r.num;
    std::vector<float> out(total);
    LtcEncoder enc(sample_rate, start.rate);
    LtcTime next = start;
    for (std::size_t pos = 0; pos < out.size();) {
        const std::size_t n = std::min(block, out.size() - pos);
        next = enc.render(next, Span<float>{out.data() + pos, n});
        pos += n;
    }
    return out;
}

struct Heard {
    std::vector<LtcTime> time;
    std::vector<std::size_t> end;   // absolute sample index
    std::uint64_t overflow = 0;
};

Heard decode(const std::vector<float>& signal, std::uint32_t sample_rate, LtcRate rate,
             std::size_t block) {
    Heard h;
    LtcDecoder dec(sample_rate, rate);
    LtcDecoded out;
    for (std::size_t pos = 0; pos < signal.size(); pos += block) {
        const std::size_t n = std::min(block, signal.size() - pos);
        dec.feed(Span<const float>{signal.data() + pos, n}, out);
        h.overflow += out.overflow;
        for (std::size_t i = 0; i < out.count; ++i) {
            h.time.push_back(out.time[i]);
            h.end.push_back(pos + out.end_offset[i]);
        }
    }
    return h;
}

// The rendered frames come back, in order, each one the successor of the
// last. A frame or two at the head may be lost while the bit clock settles,
// and the last needs the next frame's first edge; nothing else may be.
void round_trip(const LtcTime& start, std::uint32_t sample_rate) {
    constexpr std::size_t kFrames = 12;
    const std::vector<float> signal = encode(start, kFrames, sample_rate, 1024);
    const Heard h = decode(signal, sample_rate, start.rate, 1024);
    CHECK(h.time.size() >= kFrames - 3);
    CHECK(h.overflow == 0);
    for (std::size_t i = 0; i < h.time.size(); ++i) {
        CHECK(h.time[i].valid());
        if (i > 0) {
            CHECK(h.time[i].to_frames() ==
                  (h.time[i - 1].to_frames() + 1u) % frames_per_day(start.rate));
        }
    }
    if (!h.time.empty()) {
        // Where the first decoded frame sits in the rendered run is the
        // distance from the start; the labels must agree with it.
        const std::uint64_t first = h.time.front().to_frames();
        const std::uint64_t base = start.to_frames();
        const std::uint64_t day = frames_per_day(start.rate);
        const std::uint64_t k = (first + day - base) % day;
        CHECK(k < 3);
    }
}

void round_trip_every_rate() {
    for (const LtcRate rate : kRates) {
        for (const std::uint32_t sr : kSampleRates) {
            round_trip(tc(1, 2, 3, 4, rate), sr);
            // Across a minute, where drop frame skips labels.
            round_trip(tc(0, 0, 59, 20, rate), sr);
            // Across midnight.
            round_trip(tc(23, 59, 59, 20, rate), sr);
        }
    }
}

// Cutting the stream anywhere gives the same frames at the same samples.
void block_size_does_not_matter() {
    for (const LtcRate rate : kRates) {
        for (const std::uint32_t sr : kSampleRates) {
            const LtcTime start = tc(10, 20, 30, 5, rate);
            const std::vector<float> signal = encode(start, 12, sr, 4096);
            const Heard whole = decode(signal, sr, rate, signal.size());
            CHECK(!whole.time.empty());
            for (const std::size_t block : {std::size_t{1}, std::size_t{7}, std::size_t{100},
                                            std::size_t{480}, std::size_t{1920},
                                            std::size_t{4097}}) {
                const Heard cut = decode(signal, sr, rate, block);
                CHECK(cut.time.size() == whole.time.size());
                CHECK(cut.end == whole.end);
                bool same = cut.time.size() == whole.time.size();
                for (std::size_t i = 0; same && i < cut.time.size(); ++i) {
                    same = cut.time[i] == whole.time[i];
                }
                CHECK(same);
            }
        }
    }
}

// The encoder's own blocks are as free: a frame spanning two renders is
// the same waveform as one render.
void encoder_blocks_are_seamless() {
    for (const LtcRate rate : kRates) {
        const LtcTime start = tc(3, 4, 5, 6, rate);
        const std::vector<float> whole = encode(start, 6, 48'000, 100'000);
        for (const std::size_t block : {std::size_t{1}, std::size_t{37}, std::size_t{1000}}) {
            const std::vector<float> cut = encode(start, 6, 48'000, block);
            CHECK(cut == whole);
        }
    }
}

void silence_yields_no_frame() {
    for (const LtcRate rate : kRates) {
        for (const std::uint32_t sr : kSampleRates) {
            const std::vector<float> silence(sr * 2u, 0.0f);
            CHECK(decode(silence, sr, rate, 1024).time.empty());
            const std::vector<float> dc(sr, 0.4f);
            CHECK(decode(dc, sr, rate, 1024).time.empty());
        }
    }
}

// A fixed-seed generator, so the test is the same run every time.
void noise_yields_no_frame() {
    for (const LtcRate rate : kRates) {
        for (const float amp : {0.5f, 0.02f}) {
            std::uint32_t state = 12345u;
            std::vector<float> noise(48'000u * 3u);
            for (float& x : noise) {
                state = state * 1664525u + 1013904223u;
                x = amp * (static_cast<float>(state >> 8) / 8388608.0f - 1.0f);
            }
            CHECK(decode(noise, 48'000, rate, 1024).time.empty());
        }
    }
}

// Silence after a signal does not replay it, and a reset forgets the clock.
void decoder_goes_quiet_and_resets() {
    const LtcTime start = tc(1, 0, 0, 0, LtcRate::Fps25);
    std::vector<float> signal = encode(start, 8, 48'000, 1024);
    signal.insert(signal.end(), 48'000, 0.0f);
    LtcDecoder dec(48'000, LtcRate::Fps25);
    LtcDecoded out;
    dec.feed(Span<const float>{signal.data(), signal.size() - 48'000}, out);
    const std::uint64_t heard = dec.good_frames();
    CHECK(heard >= 5);
    dec.feed(Span<const float>{signal.data() + signal.size() - 48'000, 48'000}, out);
    CHECK(out.count == 0);
    CHECK(dec.good_frames() == heard);
    dec.reset();
    dec.feed(Span<const float>{signal.data(), signal.size() - 48'000}, out);
    CHECK(dec.good_frames() == heard);   // reset zeroes the count, the replay re-finds them
}

}  // namespace

int main() {
    round_trip_every_rate();
    block_size_does_not_matter();
    encoder_blocks_are_seamless();
    silence_yields_no_frame();
    noise_yields_no_frame();
    decoder_goes_quiet_and_resets();
    if (failures == 0) std::printf("ltc codec: ok\n");
    return failures == 0 ? 0 : 1;
}
