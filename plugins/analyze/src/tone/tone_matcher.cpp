// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ranking is by tone_distance_db (level-invariant weighted RMS); the
// residual EQ is the timbre pass's own fit, so the two stay one algorithm.
#include "tone_matcher.h"

#include <algorithm>
#include <set>

namespace sonora::analyze {
namespace {

bool validate_library(const std::vector<CaptureProfile>& library, std::string& error) {
    std::set<std::string> seen;
    for (const auto& c : library) {
        if (!seen.insert(c.id).second) {
            error = "capture '" + c.id + "' appears twice in the library";
            return false;
        }
        if (c.stimulus != kToneStimulusId) {
            error = "profile " + c.id + " was made with stimulus '" + c.stimulus + "', expected '" +
                    kToneStimulusId + "'";
            return false;
        }
        if (!complete(c.profile)) {
            error = "profile " + c.id + " is incomplete";
            return false;
        }
    }
    return true;
}

bool fill_suggestion(const ToneProfile& reference, const CaptureProfile& c, double distance,
                     const ToneMatchParams& params, ToneSuggestion& s, std::string& error) {
    s.capture_id = c.id;
    s.ir_id = c.ir_id;
    s.amp_settings = c.amp_settings;
    s.distance_db = distance;
    s.crest_delta_db = reference.crest_db - c.profile.crest_db;
    s.flatness_delta = reference.flatness - c.profile.flatness;
    TimbreMatchParams tp;
    tp.max_gain_db = params.max_gain_db;
    tp.smoothing_octaves = params.smoothing_octaves;
    tp.amount = 1.0;
    TimbreMatchResult r;
    if (!match_timbre(to_timbre_curve(reference), to_timbre_curve(c.profile), tp, r, error)) return false;
    s.eq_curve_db = r.curve_db;
    s.eq_bands = std::move(r.eq_bands);
    return true;
}

}  // namespace

bool ClassicalToneMatcher::match(const ToneProfile& reference, const std::vector<CaptureProfile>& library,
                                 std::vector<ToneSuggestion>& out, std::string& error) {
    out.clear();
    if (!tone_params_valid(params_)) {
        error = "tone match parameters out of range";
        return false;
    }
    if (tone_valid_core_bands(reference) < kToneMinValidCoreBands) {
        error = "reference has too few valid bands to match (too short, quiet or narrow)";
        return false;
    }
    if (!validate_library(library, error)) return false;

    std::vector<std::pair<double, const CaptureProfile*>> ranked;
    for (const auto& c : library) {
        const double d = tone_distance_db(reference, c.profile);
        if (d >= 0.0 && d <= params_.max_distance_db) ranked.emplace_back(d, &c);
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : a.second->id < b.second->id;
    });
    if (ranked.size() > static_cast<std::size_t>(params_.max_candidates))
        ranked.resize(static_cast<std::size_t>(params_.max_candidates));

    std::vector<ToneSuggestion> result;
    for (const auto& r : ranked) {
        ToneSuggestion s;
        if (!fill_suggestion(reference, *r.second, r.first, params_, s, error)) return false;
        result.push_back(std::move(s));
    }
    out = std::move(result);
    return true;
}

bool check_library_fresh(const std::vector<CaptureProfile>& library, const CardDigestLookup& digest,
                         std::string& error) {
    for (const auto& c : library) {
        std::string card, ir;
        if (!digest(c.id, card)) {
            error = "profile " + c.id + ": its card is missing";
            return false;
        }
        if (!c.ir_id.empty() && !digest(c.ir_id, ir)) {
            error = "profile " + c.id + ": the card of IR '" + c.ir_id + "' is missing";
            return false;
        }
        if (!check_fresh(c, card, ir, error)) return false;
    }
    return true;
}

}  // namespace sonora::analyze
