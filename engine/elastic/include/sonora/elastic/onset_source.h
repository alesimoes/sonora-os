// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where a region's onsets are, for Quantize Audio (ADR-0136 §4). Injected,
// so the job does not wait for the beat detector: PeakOnsetSource stands in
// until one is merged, and tests give fixed onsets. Called on the elastic
// worker, never the audio thread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sonora/stretch/stretcher.h"

namespace sonora::elastic {

class OnsetSource {
public:
    virtual ~OnsetSource() = default;
    // The onsets of audio[*][first, first + count), in frames counted from
    // `first`, inside [0, count), in any order. False, with the reason, when
    // the audio cannot be read for onsets; no onset at all is an empty list
    // and true (the planner refuses it with its own words).
    virtual bool find(const stretch::Planar& audio, std::int64_t first, std::int64_t count,
                      std::uint32_t sample_rate, std::vector<std::int64_t>& out,
                      std::string& why) = 0;
};

// The peak-envelope picker: a 5 ms energy envelope of all channels summed,
// an onset where it rises by kRiseDb over its level of 30 ms before and is
// above kFloorDb, at most one per 50 ms. Crude next to a spectral detector,
// and enough for drums and plucked parts, which is what Quantize Audio is
// mostly for.
class PeakOnsetSource final : public OnsetSource {
public:
    static constexpr double kRiseDb = 9.0;
    static constexpr double kFloorDb = -45.0;
    bool find(const stretch::Planar& audio, std::int64_t first, std::int64_t count,
              std::uint32_t sample_rate, std::vector<std::int64_t>& out,
              std::string& why) override;
};

}  // namespace sonora::elastic
