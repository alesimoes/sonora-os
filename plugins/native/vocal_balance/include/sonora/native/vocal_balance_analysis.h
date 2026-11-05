// SPDX-License-Identifier: GPL-3.0-or-later
//
// Vocal Balance, the offline half (ADR-0011 N5): a whole-track, rule-based
// analysis of a vocal, and the Plan (ADR-0038) it proposes. "AI" is the
// product name of the feature class; there is no model here (ADR-0011 4.1:
// model-assisted passes come later, in sonora-analyze). Every number a
// result holds can be explained in a sentence, and every plan step carries
// that sentence.
//
// This runs on a control thread: it allocates, holds strings and takes
// seconds on a song. It never runs in a process() path.
//
// What it measures
// - Resonances. Short-time spectra (Hann, 4096 points at 44.1/48 kHz, 8192
//   at 88.2 kHz and up, hop N/2) of the frames that carry the voice. In each
//   frame a bin's excess is its level in dB over the mean level in dB of the
//   bins within +-1/6 octave. The 30th percentile of that excess over the
//   frames is the bin's persistent excess: a resonance (a room mode, a
//   microphone or a throat peak) stays 3 dB or more above its surroundings
//   in at least 70 % of the frames, while the voice's own harmonics move
//   with the pitch and occupy a given bin for a minority of them. Peaks of
//   that curve become bands.
// - Zones. The mean spectrum against the straight line (in dB over log
//   frequency, least squares over 120 Hz to 12 kHz) through it, per zone:
//   proximity, boxiness, nasal, presence, sibilance and air. A zone more
//   than 4 dB over the line becomes a broad cut of Sonora EQ in the plan:
//   a tilt over an octave is not a resonance, and the dynamic bands' narrow
//   detectors are the wrong tool for it. A zone 4 dB or more under the
//   line is reported as a note (air in particular), never boosted.
// - Sibilance. Frames with more than half their energy between 4.5 and
//   11 kHz; their centre frequency, level, and excess over the high band of
//   the other frames.
// - Level. The median rms of the active frames and its spread.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "sonora/model/plan.h"

namespace sonora::native {

struct VocalResonance {
    double freq_hz = 0.0;
    double q = 4.0;              // measured width at half the excess, bounded 1..16
    double excess_db = 0.0;      // persistent excess (30th percentile)
    double cut_db = 0.0;         // proposed cap for the band
    double bell_q = 4.0;         // proposed sharpness of the cut
    std::string zone;            // the zone it sits in, or empty
};

struct VocalZone {
    std::string name;
    double lo_hz = 0.0, hi_hz = 0.0;
    double excess_db = 0.0;      // mean level over the line, dB
};

struct VocalSibilance {
    bool found = false;
    double freq_hz = 0.0;        // power centroid of 4.5-11 kHz in sibilant frames
    double level_db = 0.0;       // median sine-calibrated level of the band, dBFS
    double excess_db = 0.0;      // over the same band in the other active frames
    double fraction = 0.0;       // of the active frames
};

struct VocalAnalysis {
    double sample_rate = 0.0;
    double duration_s = 0.0;
    double active_fraction = 0.0;
    double level_db = -120.0;    // median rms of the active frames, dBFS
    double level_spread_db = 0.0;  // 90th minus 10th percentile of the same
    std::vector<VocalResonance> resonances;   // by frequency, at most 5
    std::vector<VocalZone> zones;
    VocalSibilance sibilance;
};

// False, with `error`, when the audio is too short (under 2 windows), too
// quiet to hold a voice (no frame above -70 dBFS), or non-finite.
bool analyse_vocal(const float* mono, std::size_t frames, double sample_rate, VocalAnalysis& out,
                   std::string& error);

// The parameter set of Sonora Vocal Balance for an analysis, as the plan
// applies it: LV2 port symbols and plain values. At least 3 bands (padded
// with default frequencies at a cap of 0, which do nothing).
std::vector<std::pair<std::string, float>> vocal_balance_params(const VocalAnalysis& a);

// The plan for one track: steps the operator can take separately.
//   - "Add Sonora Vocal Balance", with the bands found;
//   - "Add Sonora De-esser", when the sibilance stands out;
//   - "Trim", the track's fader towards a vocal level (SetMix, the one
//     value the plan can read back).
// A source never returns an empty plan as success.
class VocalBalancePlanSource : public model::PlanSource {
public:
    VocalBalancePlanSource(model::Id track, VocalAnalysis analysis, double target_level_db = -20.0)
        : track_(track), analysis_(std::move(analysis)), target_db_(target_level_db) {}
    bool propose(const model::Snapshot& at, model::Plan& out, std::string& error) override;

private:
    model::Id track_;
    VocalAnalysis analysis_;
    double target_db_;
};

}  // namespace sonora::native
