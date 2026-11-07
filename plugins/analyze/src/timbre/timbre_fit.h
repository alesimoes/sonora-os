// SPDX-License-Identifier: GPL-3.0-or-later
//
// The timbre pass's internals (ADR-0066 §4): the full result of a match,
// which carries what TimbreMatchResult (the merged contract) has no field
// for: each band's shape (bell or shelf) and the fit residual.
#pragma once

#include <string>
#include <vector>

#include "sonora/analyze/timbre_match.h"

namespace sonora::analyze {

enum class EqBandKind { Bell, LowShelf, HighShelf };

// The stock EQ's parameter ranges (plugins/native/eq/src/eq.cpp, read, not
// included: the descriptor is not a header). Every emitted band is inside.
constexpr double kEqBellFreqMin = 20.0, kEqBellFreqMax = 20000.0;
constexpr double kEqBellQMin = 0.1, kEqBellQMax = 18.0;
constexpr double kEqLowShelfFreqMin = 20.0, kEqLowShelfFreqMax = 2000.0;
constexpr double kEqHighShelfFreqMin = 1000.0, kEqHighShelfFreqMax = 20000.0;
constexpr double kEqShelfQ = 0.7071067811865476, kEqShelfQMin = 0.5, kEqShelfQMax = 1.5;
constexpr double kEqGainLimitDb = 24.0;
constexpr int kTimbreMaxFitBands = 8;

struct TimbreFit {
    TimbreMatchResult result;
    std::vector<EqBandKind> kinds;  // parallel to result.eq_bands
    double curve_rms_db = 0.0;      // of the target curve, over the valid bands
    double residual_rms_db = 0.0;   // of (curve - fitted response), same bands
    std::array<bool, kTimbreBands> valid{};
};

// match_timbre with the shapes and the residual. Same errors.
bool match_timbre_detailed(const TimbreCurve& reference, const TimbreCurve& target, const TimbreMatchParams& params,
                           TimbreFit& out, std::string& error);

// Response in dB of a set of bands (analog prototypes, RBJ cookbook shapes,
// the stock EQ's design target) at f_hz.
double eq_bands_response_db(const std::vector<EqBand>& bands, const std::vector<EqBandKind>& kinds, double f_hz);

// The pieces, for tests.
void fit_eq_bands(const std::array<double, kTimbreBands>& curve_db, const std::array<bool, kTimbreBands>& valid,
                  int max_bands, double max_gain_db, TimbreFit& out);

// Welch long-term spectrum (ADR-0066 §4): 8192-point Hann frames at 50 %
// overlap, frames below -70 dBFS or 20 dB under the mean of the rest
// dropped, 31 third-octave bands, level-normalised to the total energy.
class WelchTimbreMeasurer final : public TimbreMeasurer {
public:
    bool measure(AudioSource& source, const RunOptions& run, TimbreCurve& out, std::string& error) override;
};

}  // namespace sonora::analyze
