// SPDX-License-Identifier: GPL-3.0-or-later
//
// Monophonic pitch tracking (time.pitch, ADR-0143 section 3). A tracker turns
// mono samples into a contour: one frame per hop with voiced/unvoiced and
// cents against A4. Pure and deterministic, off the audio thread.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sonora::pitch {

// 0 cents is A4 (a4_hz); 100 cents a semitone, so -900 is C4.
inline double hz_to_cents(double hz, double a4_hz) { return 1200.0 * std::log2(hz / a4_hz); }
inline double cents_to_hz(double cents, double a4_hz) { return a4_hz * std::exp2(cents / 1200.0); }

struct PitchFrame {
    float cents = 0.0f;       // meaningful only when voiced
    float confidence = 0.0f;  // 1 - the YIN aperiodicity at the chosen lag, 0..1
    bool voiced = false;
};

// Frame i is centred on sample i * hop (frame 0 on the first sample), so a
// contour covers the whole input with length / hop + 1 frames.
struct Contour {
    uint32_t rate = 0;
    std::size_t hop = 0;
    std::size_t length = 0;  // input samples
    double a4_hz = 440.0;
    std::vector<PitchFrame> frames;

    std::size_t voiced_count() const {
        std::size_t n = 0;
        for (const PitchFrame& f : frames) n += f.voiced ? 1 : 0;
        return n;
    }
    double frame_ms() const { return rate == 0 ? 0.0 : 1000.0 * double(hop) / double(rate); }
};

class PitchTracker {
public:
    virtual ~PitchTracker() = default;
    // False with `why` set (a reason the operator can read) when the input or
    // the configuration cannot be tracked; `out` is then untouched.
    virtual bool track(const std::vector<float>& mono, uint32_t rate, Contour& out,
                       std::string& why) = 0;
};

struct YinConfig {
    double a4_hz = 440.0;     // 400..480
    double hop_ms = 5.0;      // 5..10
    double min_hz = 70.0;     // lowest pitch looked for
    double max_hz = 1000.0;   // highest
    double threshold = 0.15;  // absolute CMNDF threshold, YIN paper step 4
    double gate_db = -55.0;   // frames quieter than this (RMS, dBFS) are unvoiced
};

// YIN (de Cheveigne and Kawahara, JASA 111(4), 2002): difference function,
// cumulative mean normalisation, absolute threshold, parabolic interpolation.
// Input above 16 kHz is box-averaged down by an integer factor first: pitch
// needs no more, and the cost per frame falls with the square of it.
class YinTracker : public PitchTracker {
public:
    explicit YinTracker(const YinConfig& config = {}) : config_(config) {}
    bool track(const std::vector<float>& mono, uint32_t rate, Contour& out,
               std::string& why) override;

private:
    YinConfig config_;
};

}  // namespace sonora::pitch
