// SPDX-License-Identifier: GPL-3.0-or-later
//
// The rule-based mastering suggestion pass (ADR-0011 N4, ADR-0046): from a
// MixAnalysis to settings for the mastering console. Rules, not a model:
// the same analysis always gives the same settings, and every setting
// carries the measurement and the rule behind it, in words the operator can
// check against the meters.
//
// The rules (each a Finding when it changes something):
//  1. Spectral balance. The octave-band levels are put on the same footing
//     as the target curve (MasteringRules::target_db) by anchoring both to the mean of the
//     500 Hz, 1 kHz and 2 kHz bands, so a mix is judged on its shape and not
//     its level. Bands within +-tolerance of the curve are left alone; past
//     it, the console takes out half of the excess (a correction, not a
//     match), at most 6 dB, in steps of 0.5 dB:
//       63 + 125 Hz  -> low shelf (100 Hz)
//       250 Hz       -> low-mid bell (250 Hz)
//       4 kHz        -> high-mid bell (4 kHz)
//       8 + 16 kHz   -> high shelf (10 kHz)
//     31.5 Hz and the anchor bands are reported, not adjusted.
//  2. Dynamics, from peak-to-loudness ratio (PLR = true peak - integrated
//     loudness). Below 8 dB the mix is dense: the compressor stays at 1:1,
//     saturation off, and the loudness rule below is held to the headroom it
//     has. Above 14 dB it is dynamic: a gentle compressor (1.5:1, 2:1 above
//     18 dB) from 4 dB above the programme loudness. Above 16 dB, 15 %
//     saturation at 6 dB drive rounds the peaks as well.
//  3. Stereo. A correlation below 0 (more out of phase than in) narrows the
//     width to 80 %; a side level within 3 dB of the mid narrows it to 90 %.
//     A near-mono mix (side more than 24 dB under the mid) is reported and
//     left alone: widening it would change the balance.
//  4. Loudness. The gain ahead of the limiter is target - integrated
//     loudness, within +-12 dB. To go louder the limiter takes what the
//     headroom (ceiling - true peak) does not cover, but never more than
//     max_limiting_db: when that caps the gain, the pass says so and what
//     loudness it reaches. A mix louder than target is turned down by the
//     gain, and past its -12 dB by the strip's fader.
//  5. Ceiling and target are set from the config, always.
//
// The target curve is an engineering target for the long-term octave
// spectrum of a finished popular-music mix: flat to a gentle bass
// emphasis and then falling about 4 dB per octave above 1 kHz. It follows
// the shape reported for commercial releases by Pestana, Reiss and Barbosa,
// "Spectral Characteristics of Popular Commercial Recordings 1950-2010",
// AES 135th Convention, 2013, and the practice in B. Katz, "Mastering
// Audio", 3rd ed., Focal Press, 2015; it is not taken from any product.
#pragma once

#include <string>
#include <vector>

#include "sonora/analyze/mix_analysis.h"
#include "sonora/native/mastering.h"

namespace sonora::analyze {

// One spectral-balance rule: which octave bands it judges and the console
// parameters it moves. Loaded from data/mastering_rules.json.
struct BalanceGroup {
    std::string rule;         // "balance.low"
    int bands[2] = {-1, -1};  // octave-band indices, -1 for unused
    std::string what;         // "63 and 125 Hz"
    std::string title;        // "Low shelf"
    std::string at;           // "at 100 Hz"
    std::uint32_t gain = 0;   // native::mastering::k... index
    std::uint32_t freq = 0;   // 0: fixed frequency
    double freq_hz = 0.0;
};

// The rule table (ADR-0172): the target curve, the thresholds and the
// balance groups, as data. parse/load fail with a message on a missing or
// malformed file; there is no built-in fallback.
struct MasteringRules {
    // dB per octave band, relative to the 1 kHz band. Only its shape is used:
    // the pass anchors both it and the mix to the mean of the anchor bands.
    double target_db[kBands] = {};
    std::string band_names[kBands];
    std::vector<int> anchor_bands;
    double sub_bass_margin_db = 0.0;
    std::vector<BalanceGroup> groups;
    double ratio_step_plr_db = 0.0, ratio_normal = 0.0, ratio_high = 0.0;
    double threshold_above_loudness_db = 0.0;
    double saturation_plr_db = 0.0, saturation_drive_db = 0.0, saturation_mix_pct = 0.0;
    double phase_correlation = 0.0, phase_width_pct = 0.0;
    double wide_side_mid_db = 0.0, wide_width_pct = 0.0, near_mono_side_mid_db = 0.0;
    double no_change_lu = 0.0, dense_limit_db = 0.0;
};

bool parse_mastering_rules(const std::string& json_text, MasteringRules& out, std::string& error);
bool load_mastering_rules(const std::string& path, MasteringRules& out, std::string& error);

struct MasteringConfig {
    // The loaded table; suggest_mastering refuses a config without one.
    const MasteringRules* table = nullptr;
    double target_lufs = -14.0;     // streaming; -23 for EBU R128 broadcast
    double ceiling_dbtp = -1.0;
    double max_limiting_db = 5.0;   // most the limiter is asked to take
    double band_tolerance_db = 3.0;
    double eq_fraction = 0.5;
    double eq_max_db = 6.0;
    double dense_plr_db = 8.0;
    double dynamic_plr_db = 14.0;
};

struct Setting {
    std::uint32_t index;  // native::mastering::k...
    float value;          // plain units
};

// One decision: a heading, the measurement and rule behind it, and the
// console settings it sets.
struct Finding {
    std::string rule;     // "balance.low", "dynamics.dense", "loudness.gain"
    std::string title;
    std::string reason;
    std::vector<Setting> settings;
};

struct MasteringSuggestion {
    // The console's complete settings, by parameter index: the descriptor's
    // defaults with the findings applied.
    std::vector<float> values;
    // What the pass changed or decided, in rule order.
    std::vector<Finding> findings;
    // What it measured and left alone, in words.
    std::vector<std::string> observations;
    // A gain change on the strip's fader, in dB (zero when none): the part
    // of a turn-down the console's gain cannot take.
    double fader_trim_db = 0.0;
    std::string fader_reason;
    // The loudness the settings aim for, before the EQ's and compressor's
    // own effect on level.
    double aimed_lufs = 0.0;
};

// False, with `error`, for an analysis that is not usable (silent, or
// inconsistent).
bool suggest_mastering(const MixAnalysis& analysis, const MasteringConfig& config, MasteringSuggestion& out,
                       std::string& error);

}  // namespace sonora::analyze
