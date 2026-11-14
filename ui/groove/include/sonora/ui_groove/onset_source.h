// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where a reference region's hits are, for a groove taken from it (ADR-0115).
// The interface is injected so the groove command is tested on fixed onsets;
// the real one reads the waveform overview (PeakSource), never the audio, so
// it answers from memory on the GUI thread.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "sonora/model/model.h"

namespace sonora::ui {

class PeakSource;

class OnsetSource {
public:
    virtual ~OnsetSource() = default;
    // Timeline frames where something starts in `r`, ascending. False, with
    // the reason, when it cannot tell: an onset list it is not sure of would
    // put every target on a wrong groove.
    virtual bool onsets(const model::Region& r, std::uint32_t sample_rate,
                        std::vector<model::Frames>& out, std::string& why) const = 0;
};

struct OnsetParams {
    double bucket_ms = 5.0;       // the envelope's resolution
    double threshold_db = -18.0;  // below the region's loudest bucket
    double release_db = -30.0;    // the envelope must fall here before the next hit
    double min_gap_ms = 60.0;     // two hits closer than this are one
};

// A peak-envelope threshold with hysteresis over the overview of every
// channel of the region. Refuses a source whose overview is still being read
// (it answers silence, which would read as no hits) and a region with no
// sound at all.
class PeakOnsetSource final : public OnsetSource {
public:
    explicit PeakOnsetSource(const PeakSource& peaks, OnsetParams params = {})
        : peaks_(peaks), params_(params) {}
    bool onsets(const model::Region& r, std::uint32_t sample_rate,
                std::vector<model::Frames>& out, std::string& why) const override;

private:
    const PeakSource& peaks_;
    OnsetParams params_;
};

// Onsets given per region id, for the tests and the screenshots. A region
// it was not given is refused, as the real one refuses what it cannot read.
class FixedOnsetSource final : public OnsetSource {
public:
    void set(model::Id region, std::vector<model::Frames> onsets) {
        onsets_[region] = std::move(onsets);
    }
    bool onsets(const model::Region& r, std::uint32_t, std::vector<model::Frames>& out,
                std::string& why) const override {
        const auto it = onsets_.find(r.id);
        if (it == onsets_.end()) {
            why = "no onsets are known for " + r.name;
            return false;
        }
        out = it->second;
        return true;
    }

private:
    std::map<model::Id, std::vector<model::Frames>> onsets_;
};

}  // namespace sonora::ui
