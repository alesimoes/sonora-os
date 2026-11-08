// SPDX-License-Identifier: GPL-3.0-or-later
//
// The classical matcher (ADR-0066 §4): the per-band difference, its median
// removed (a residual level offset is not timbre, and a median survives a
// few boosted or cut regions where a mean would not), clamped, smoothed,
// scaled by `amount`, then fitted to at most `max_bands` EQ bands.
#include <algorithm>
#include <cmath>

#include "timbre_fit.h"

namespace sonora::analyze {
namespace {

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// Triangular kernel of radius ceil(octaves * 3 / 2) bands over valid
// neighbours only, so an invalid band never pulls the curve to zero.
std::array<double, kTimbreBands> smooth(const std::array<double, kTimbreBands>& x,
                                        const std::array<bool, kTimbreBands>& valid, double octaves) {
    const int radius = static_cast<int>(std::ceil(octaves * 1.5 - 1e-9));
    std::array<double, kTimbreBands> y{};
    for (int i = 0; i < kTimbreBands; ++i) {
        if (!valid[i]) continue;
        double num = 0.0, den = 0.0;
        for (int k = -radius; k <= radius; ++k) {
            const int j = i + k;
            if (j < 0 || j >= kTimbreBands || !valid[j]) continue;
            const double w = radius + 1 - std::abs(k);
            num += w * x[j];
            den += w;
        }
        y[i] = num / den;
    }
    return y;
}

}  // namespace

bool match_timbre_detailed(const TimbreCurve& reference, const TimbreCurve& target, const TimbreMatchParams& params,
                           TimbreFit& out, std::string& error) {
    if (!timbre_params_valid(params) || params.max_bands > kTimbreMaxFitBands) {
        error = "timbre parameters out of range";
        return false;
    }
    std::array<bool, kTimbreBands> valid{};
    std::array<double, kTimbreBands> diff{};
    std::vector<double> values;
    for (int i = 0; i < kTimbreBands; ++i) {
        valid[i] = reference.valid[i] && target.valid[i];
        if (!valid[i]) continue;
        diff[i] = reference.gain_db[i] - target.gain_db[i];
        values.push_back(diff[i]);
    }
    if (values.empty()) {
        error = "no band is valid in both curves";
        return false;
    }
    const double offset = median(values);
    for (int i = 0; i < kTimbreBands; ++i)
        if (valid[i]) diff[i] = std::clamp(diff[i] - offset, -params.max_gain_db, params.max_gain_db);
    auto curve = smooth(diff, valid, params.smoothing_octaves);
    for (int i = 0; i < kTimbreBands; ++i)
        if (valid[i]) curve[i] = std::clamp(curve[i] * params.amount, -params.max_gain_db, params.max_gain_db);
    out.result.curve_db = curve;
    fit_eq_bands(curve, valid, params.max_bands, params.max_gain_db, out);
    return true;
}

bool match_timbre(const TimbreCurve& reference, const TimbreCurve& target, const TimbreMatchParams& params,
                  TimbreMatchResult& out, std::string& error) {
    TimbreFit fit;
    if (!match_timbre_detailed(reference, target, params, fit, error)) return false;
    out = fit.result;
    return true;
}

}  // namespace sonora::analyze
