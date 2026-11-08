// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reference-tone matching (ADR-0072): which installed captures come
// closest to the spectrum of a reference the user supplies, and the EQ
// that closes the rest of the gap. A suggestion is settings only: a
// capture id, a cabinet IR id, the card's amp settings and an EQ curve.
// It never names an artist, band or product; the reference is labelled by
// the user's own file name, echoed verbatim and never matched against
// anything. The operator previews a suggestion and applies it as an
// undoable session operation (ADR-0011 §4.1, §4.4).
//
// Offline only (sonora-analyze); nothing here is reachable from a
// `process`.
#pragma once

#include <array>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "sonora/analyze/timbre_match.h"
#include "sonora/analyze/tone_profile.h"

namespace sonora::analyze {

// What the report says instead of a candidate list (ADR-0072 §4, §5).
// Both are exit 0: an honest empty answer, never a forced match.
constexpr const char* kToneNoCapturesInstalled = "no captures installed";
constexpr const char* kToneNoCloseCapture = "no close capture";

struct ToneMatchParams {
    int max_candidates = 3;
    // A candidate further than this is not suggested. Weighted RMS of the
    // level-aligned band differences, dB. A starting value, to be tuned
    // on the first real captures (ADR-0072, Unverified).
    double max_distance_db = 6.0;
    // The residual EQ: clamped to +/- this, smoothed over this width.
    double max_gain_db = 12.0;
    double smoothing_octaves = 1.0 / 3.0;
};

// A reference with fewer valid bands than this among the weighted ones
// (80 Hz to 6.3 kHz, 20 bands) is too short, too quiet or too narrow to
// match; the matcher fails rather than guess.
constexpr int kToneMinValidCoreBands = 16;

struct ToneSuggestion {
    std::string capture_id;
    std::string ir_id;  // empty: the capture includes its cabinet
    std::vector<std::pair<std::string, std::string>> amp_settings;  // the card's
    double distance_db = 0.0;
    // reference minus capture, for the operator's information: crest hints
    // at drive and compression, flatness at fizz.
    double crest_delta_db = 0.0;
    double flatness_delta = 0.0;
    // The residual: reference minus capture per band, level-aligned,
    // smoothed and clamped by the params; 0 where the reference band is
    // invalid. Exactly match_timbre(to_timbre_curve(reference),
    // to_timbre_curve(capture), {max_gain_db, smoothing_octaves, amount 1})
    // so the existing EQ fit turns it into eq_bands for the stock EQ.
    std::array<double, kToneBands> eq_curve_db{};
    std::vector<EqBand> eq_bands;
};

// Weights favour the guitar's useful range: full weight for the bands
// centred 80 Hz to 6.3 kHz, a quarter outside (room rumble below, cabinet
// roll-off and noise above).
inline double tone_band_weight(int band) noexcept {
    const double f = kThirdOctaveCentreHz[band];
    return (f >= 80.0 && f <= 6300.0) ? 1.0 : 0.25;
}

inline int tone_valid_core_bands(const ToneProfile& p) noexcept {
    int n = 0;
    for (int b = 0; b < kToneBands; ++b)
        if (p.valid[b] && tone_band_weight(b) == 1.0) ++n;
    return n;
}

// The distance of ADR-0072 §4: weighted L2 over the bands valid in both,
// after removing the weighted mean difference (overall level is not tone),
// normalised by the total weight so it reads as an RMS in dB. Negative
// when no band is valid in both.
inline double tone_distance_db(const ToneProfile& reference, const ToneProfile& capture) noexcept {
    double wsum = 0.0, mean = 0.0;
    for (int b = 0; b < kToneBands; ++b) {
        if (!reference.valid[b] || !capture.valid[b]) continue;
        const double w = tone_band_weight(b);
        wsum += w;
        mean += w * (reference.bands_db[b] - capture.bands_db[b]);
    }
    if (wsum <= 0.0) return -1.0;
    mean /= wsum;
    double acc = 0.0;
    for (int b = 0; b < kToneBands; ++b) {
        if (!reference.valid[b] || !capture.valid[b]) continue;
        const double d = reference.bands_db[b] - capture.bands_db[b] - mean;
        acc += tone_band_weight(b) * d * d;
    }
    return std::sqrt(acc / wsum);
}

// What the app is written against; the classical implementation is the
// engine slice's, its params fixed at construction.
//
// match() fails (false, `error` set) when the reference has fewer than
// kToneMinValidCoreBands valid core bands, or any library entry is not
// complete, carries another stimulus, or repeats an id: one bad profile
// fails the pass, it is never skipped. Freshness against the cards
// (check_fresh) is the loader's, before the library reaches here.
// Otherwise true, with `out` replaced by at most max_candidates
// suggestions within max_distance_db, nearest first, ties by capture id.
// An empty `out` is a valid answer: kToneNoCapturesInstalled when the
// library is empty, else kToneNoCloseCapture.
class ToneMatcher {
public:
    virtual ~ToneMatcher() = default;
    virtual bool match(const ToneProfile& reference, const std::vector<CaptureProfile>& library,
                       std::vector<ToneSuggestion>& out, std::string& error) = 0;
};

inline bool tone_params_valid(const ToneMatchParams& p) noexcept {
    return p.max_candidates >= 1 && p.max_candidates <= 10 && p.max_distance_db > 0.0 &&
           p.max_gain_db > 0.0 && p.max_gain_db <= 24.0 && p.smoothing_octaves >= 0.0 &&
           p.smoothing_octaves <= 2.0;
}

}  // namespace sonora::analyze
