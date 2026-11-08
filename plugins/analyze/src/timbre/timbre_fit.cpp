// SPDX-License-Identifier: GPL-3.0-or-later
//
// Greedy parametric fit of a third-octave difference curve (ADR-0066 §4).
// Candidates are bells at every valid band centre over a small Q set and a
// shelf at each end; each step adds the candidate that removes the most
// squared error (gain by least squares on the linearised dB shape, then
// re-evaluated exactly), then the gains of all chosen bands are refined.
// The responses are the analog prototypes (dsp::analog_response, RBJ
// cookbook), which is what the stock EQ's "analog matched" mode targets.
#include <algorithm>
#include <cmath>

#include "sonora/dsp/biquad.h"
#include "timbre_fit.h"

namespace sonora::analyze {
namespace {

constexpr double kMinBandGainDb = 0.25;   // under this a band is not worth a control
constexpr double kMinImprovement = 0.02;  // dB^2 summed over bands: stop below this
constexpr double kBellQs[] = {0.7, 1.0, 1.4, 2.0, 3.0, 4.5, 7.0};

using Curve = std::array<double, kTimbreBands>;
using Valid = std::array<bool, kTimbreBands>;

sonora::dsp::Shape shape_of(EqBandKind k) {
    switch (k) {
        case EqBandKind::LowShelf: return sonora::dsp::Shape::LowShelf;
        case EqBandKind::HighShelf: return sonora::dsp::Shape::HighShelf;
        default: return sonora::dsp::Shape::Peak;
    }
}

double band_db(EqBandKind k, double f0, double q, double gain_db, double f) {
    sonora::dsp::FilterSpec s;
    s.shape = shape_of(k);
    s.freq_hz = f0;
    s.q = q;
    s.gain_db = gain_db;
    return 20.0 * std::log10(std::abs(sonora::dsp::analog_response(s, f)));
}

struct Cand {
    EqBandKind kind;
    double freq, q;
    Curve shape{};  // dB per dB of gain, at the band centres
};

bool in_range(EqBandKind k, double f) {
    switch (k) {
        case EqBandKind::LowShelf: return f >= kEqLowShelfFreqMin && f <= kEqLowShelfFreqMax;
        case EqBandKind::HighShelf: return f >= kEqHighShelfFreqMin && f <= kEqHighShelfFreqMax;
        default: return f >= kEqBellFreqMin && f <= kEqBellFreqMax;
    }
}

std::vector<Cand> make_candidates(const Valid& valid) {
    std::vector<Cand> out;
    for (int b = 0; b < kTimbreBands; ++b) {
        if (!valid[b]) continue;
        const double f = kThirdOctaveCentreHz[b];
        auto add = [&](EqBandKind k, double q) {
            if (!in_range(k, f)) return;
            Cand c{k, f, q, {}};
            for (int i = 0; i < kTimbreBands; ++i) c.shape[i] = band_db(k, f, q, 6.0, kThirdOctaveCentreHz[i]) / 6.0;
            out.push_back(c);
        };
        for (double q : kBellQs) add(EqBandKind::Bell, q);
        add(EqBandKind::LowShelf, kEqShelfQ);
        add(EqBandKind::HighShelf, kEqShelfQ);
    }
    return out;
}

double sum_sq(const Curve& r, const Valid& valid) {
    double s = 0.0;
    for (int i = 0; i < kTimbreBands; ++i)
        if (valid[i]) s += r[i] * r[i];
    return s;
}

// Least-squares gain of `shape` against `r` over the valid bands, clamped.
double ls_gain(const Curve& shape, const Curve& r, const Valid& valid, double limit) {
    double num = 0.0, den = 0.0;
    for (int i = 0; i < kTimbreBands; ++i)
        if (valid[i]) {
            num += r[i] * shape[i];
            den += shape[i] * shape[i];
        }
    if (den <= 0.0) return 0.0;
    return std::clamp(num / den, -limit, limit);
}

struct Chosen {
    const Cand* cand;
    double gain;
};

Curve residual_of(const Curve& curve, const std::vector<Chosen>& chosen) {
    Curve r = curve;
    for (int i = 0; i < kTimbreBands; ++i)
        for (const Chosen& c : chosen)
            r[i] -= band_db(c.cand->kind, c.cand->freq, c.cand->q, c.gain, kThirdOctaveCentreHz[i]);
    return r;
}

// The best next band for the residual `r`, or false when none is worth it.
bool pick_next(const std::vector<Cand>& cands, const Curve& r, const Valid& valid, double limit, Chosen& pick) {
    const double before = sum_sq(r, valid);
    double best_after = before;
    pick = {nullptr, 0.0};
    for (const Cand& c : cands) {
        const double g = ls_gain(c.shape, r, valid, limit);
        if (std::fabs(g) < kMinBandGainDb) continue;
        double after = 0.0;
        for (int i = 0; i < kTimbreBands; ++i)
            if (valid[i]) {
                const double e = r[i] - band_db(c.kind, c.freq, c.q, g, kThirdOctaveCentreHz[i]);
                after += e * e;
            }
        if (after < best_after) {
            best_after = after;
            pick = {&c, g};
        }
    }
    return pick.cand && before - best_after >= kMinImprovement;
}

}  // namespace

double eq_bands_response_db(const std::vector<EqBand>& bands, const std::vector<EqBandKind>& kinds, double f_hz) {
    double db = 0.0;
    for (std::size_t i = 0; i < bands.size(); ++i)
        db += band_db(kinds[i], bands[i].freq_hz, bands[i].q, bands[i].gain_db, f_hz);
    return db;
}

void fit_eq_bands(const Curve& curve_db, const Valid& valid, int max_bands, double max_gain_db, TimbreFit& out) {
    out.result.eq_bands.clear();
    out.kinds.clear();
    out.valid = valid;
    out.curve_rms_db = 0.0;
    out.residual_rms_db = 0.0;
    int nvalid = 0;
    for (bool v : valid) nvalid += v ? 1 : 0;
    if (nvalid == 0) return;
    const double limit = std::min(max_gain_db, kEqGainLimitDb);
    out.curve_rms_db = std::sqrt(sum_sq(curve_db, valid) / nvalid);

    const std::vector<Cand> cands = make_candidates(valid);
    std::vector<Chosen> chosen;
    while (static_cast<int>(chosen.size()) < max_bands) {
        Chosen pick{};
        if (!pick_next(cands, residual_of(curve_db, chosen), valid, limit, pick)) break;
        chosen.push_back(pick);
        for (int sweep = 0; sweep < 3; ++sweep)  // refine every gain against the others
            for (std::size_t j = 0; j < chosen.size(); ++j) {
                chosen[j].gain = 0.0;
                chosen[j].gain = ls_gain(chosen[j].cand->shape, residual_of(curve_db, chosen), valid, limit);
            }
    }

    // Drop bands the refinement reduced to nothing; order low to high.
    chosen.erase(std::remove_if(chosen.begin(), chosen.end(),
                                [](const Chosen& c) { return std::fabs(c.gain) < kMinBandGainDb; }),
                 chosen.end());
    std::sort(chosen.begin(), chosen.end(), [](const Chosen& a, const Chosen& b) { return a.cand->freq < b.cand->freq; });
    for (const Chosen& c : chosen) {
        out.result.eq_bands.push_back({c.cand->freq, c.gain, c.cand->q});
        out.kinds.push_back(c.cand->kind);
    }
    out.residual_rms_db = std::sqrt(sum_sq(residual_of(curve_db, chosen), valid) / nvalid);
}

}  // namespace sonora::analyze
