// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fakes for reference-tone matching (ADR-0072), so the engine slice's and
// the command's tests run without audio, cards or capture renders: a
// matcher that returns what it was set up with, and builders for valid
// and stale capture profiles.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "sonora/analyze/tone_match.h"
#include "sonora/analyze/tone_profile.h"

namespace sonora::analyze {

// A complete profile whose bands follow a straight tilt of `tilt_db` per
// band about the middle band (so the mean is 0), every band valid.
inline ToneProfile fake_tone_profile(double tilt_db = 0.0, double crest_db = 12.0, double flatness = 0.25) {
    ToneProfile p;
    for (int b = 0; b < kToneBands; ++b) {
        p.bands_db[b] = tilt_db * static_cast<double>(b - kToneBands / 2);
        p.valid[b] = true;
    }
    p.crest_db = crest_db;
    p.flatness = flatness;
    return p;
}

// A fresh profile: stamped with `card_sha256`, made with the current
// stimulus, no separate IR.
inline CaptureProfile fake_capture_profile(const std::string& id, const std::string& card_sha256,
                                           double tilt_db = 0.0) {
    CaptureProfile c;
    c.id = id;
    c.card_sha256 = card_sha256;
    c.stimulus = kToneStimulusId;
    c.profile = fake_tone_profile(tilt_db);
    c.amp_settings = {{"gain", "6"}, {"bass", "5"}, {"middle", "5"}, {"treble", "5"}};
    return c;
}

// Stale against `card_sha256`: stamped with another digest, as a profile
// is after its card was edited.
inline CaptureProfile fake_stale_capture_profile(const std::string& id, const std::string& card_sha256) {
    CaptureProfile c = fake_capture_profile(id, card_sha256);
    c.card_sha256 = std::string(64, card_sha256.empty() || card_sha256[0] != '0' ? '0' : '1');
    return c;
}

class FakeToneMatcher final : public ToneMatcher {
public:
    std::vector<ToneSuggestion> suggestions;
    std::string fail_with;  // non-empty: match fails with this error
    int calls = 0;
    ToneProfile last_reference;
    std::size_t last_library_size = 0;

    bool match(const ToneProfile& reference, const std::vector<CaptureProfile>& library,
               std::vector<ToneSuggestion>& out, std::string& error) override {
        ++calls;
        last_reference = reference;
        last_library_size = library.size();
        if (!fail_with.empty()) {
            error = fail_with;
            return false;
        }
        out = library.empty() ? std::vector<ToneSuggestion>{} : suggestions;
        return true;
    }
};

}  // namespace sonora::analyze
