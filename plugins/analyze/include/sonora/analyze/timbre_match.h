// SPDX-License-Identifier: GPL-3.0-or-later
//
// The timbre pass (ADR-0066 §4): the long-term third-octave balance of a
// reference and a target, and the parametric EQ settings that move the
// target towards the reference. A reference is reduced to its curve and
// closed; no audio of it is stored or written. The EQ bands are settings
// for the stock EQ, applied by the operator as an undoable operation.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "sonora/analyze/mix_analysis.h"

namespace sonora::analyze {

// ISO 266 preferred third-octave centres, 20 Hz to 20 kHz.
constexpr int kTimbreBands = 31;
constexpr double kThirdOctaveCentreHz[kTimbreBands] = {
    20.0,   25.0,   31.5,   40.0,   50.0,   63.0,   80.0,    100.0,   125.0,   160.0,   200.0,
    250.0,  315.0,  400.0,  500.0,  630.0,  800.0,  1000.0,  1250.0,  1600.0,  2000.0,  2500.0,
    3150.0, 4000.0, 5000.0, 6300.0, 8000.0, 10000.0, 12500.0, 16000.0, 20000.0};

// dB, relative: only differences between bands mean anything. A band
// whose edge is beyond Nyquist, or that held no frames above the silence
// gate, is not valid and is reported as null.
struct TimbreCurve {
    std::array<double, kTimbreBands> gain_db{};
    std::array<bool, kTimbreBands> valid{};
};

struct EqBand {
    double freq_hz = 1000.0;
    double gain_db = 0.0;
    double q = 1.0;
};

struct TimbreMatchParams {
    int max_bands = 6;            // the stock EQ's bands
    double max_gain_db = 12.0;    // per band, either sign
    double amount = 1.0;          // 0 to 1: how much of the difference to correct
    double smoothing_octaves = 1.0 / 3.0;
};

struct TimbreMatchResult {
    // reference - target per band after smoothing and scaling by `amount`;
    // 0 where either curve is invalid.
    std::array<double, kTimbreBands> curve_db{};
    std::vector<EqBand> eq_bands;  // at most params.max_bands
};

// The classical matcher, implemented by the timbre slice. False with
// `error` when the params are out of range or no band is valid in both.
bool match_timbre(const TimbreCurve& reference, const TimbreCurve& target, const TimbreMatchParams& params,
                  TimbreMatchResult& out, std::string& error);

// What the app is written against, so its tests use the fakes.
class TimbreMeasurer {
public:
    virtual ~TimbreMeasurer() = default;
    virtual bool measure(AudioSource& source, const RunOptions& run, TimbreCurve& out, std::string& error) = 0;
};

class TimbreMatcher {
public:
    virtual ~TimbreMatcher() = default;
    virtual bool match(const TimbreCurve& reference, const TimbreCurve& target, const TimbreMatchParams& params,
                       TimbreMatchResult& out, std::string& error) = 0;
};

inline bool timbre_params_valid(const TimbreMatchParams& p) noexcept {
    return p.max_bands >= 1 && p.max_bands <= kTimbreBands && p.max_gain_db > 0.0 && p.max_gain_db <= 24.0 &&
           p.amount >= 0.0 && p.amount <= 1.0 && p.smoothing_octaves >= 0.0 && p.smoothing_octaves <= 2.0;
}

}  // namespace sonora::analyze
