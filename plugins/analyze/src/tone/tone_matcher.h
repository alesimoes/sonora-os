// SPDX-License-Identifier: GPL-3.0-or-later
//
// The classical tone matcher (ADR-0072 §4): weighted band distance, the
// nearest captures, the residual EQ, and the staleness check of a whole
// library. Pure functions; allocation is fine (off the audio path).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "sonora/analyze/tone_match.h"

namespace sonora::analyze {

class ClassicalToneMatcher final : public ToneMatcher {
public:
    explicit ClassicalToneMatcher(ToneMatchParams params = {}) : params_(params) {}
    bool match(const ToneProfile& reference, const std::vector<CaptureProfile>& library,
               std::vector<ToneSuggestion>& out, std::string& error) override;

private:
    ToneMatchParams params_;
};

// The SHA-256 of a card as it is now, by id; false when the card is gone.
using CardDigestLookup = std::function<bool(const std::string& id, std::string& sha256)>;

// Refuses the whole library at the first profile whose stamps differ from
// its cards' current digests (or whose card is missing), with the capture
// id in `error`. Never skips a profile: a stale one fails the pass.
bool check_library_fresh(const std::vector<CaptureProfile>& library, const CardDigestLookup& digest,
                         std::string& error);

}  // namespace sonora::analyze
