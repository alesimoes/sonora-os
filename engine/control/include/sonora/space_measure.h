// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>

#include "sonora/mix_planner.h"

namespace sonora {

// What a reference recording says about its reverb and delay space, measured
// offline (ADR-0045 8); fills the ReverbDelayReference the operator used to
// type. Each estimate has a `found` flag: a reference without a clean decay,
// an isolated transient or a repeating echo leaves that field at the
// planner's default and says so, it never guesses.
//
// Never on an audio thread: it allocates, and reads whole files.
struct SpaceMeasure {
    double sample_rate = 0.0;
    double duration_s = 0.0;

    bool decay_found = false;     // free decays: Schroeder backward integration
    double rt60_s = 0.0;          // T20 (-5..-25 dB) x 3, median of the decays
    double edt_s = 0.0;           // early decay time (0..-10 dB) x 6, median
    int decay_count = 0;

    bool predelay_found = false;  // isolated transient, gap, then the tail
    double predelay_ms = 0.0;
    bool drr_found = false;       // same transients: direct vs. reverberant energy
    double drr_db = 0.0;          // 10 log10(direct / reverberant)
    double reverb_wet_db = 0.0;   // -drr, the return under the dry, clamped to [-40, 0]

    bool echo_found = false;      // autocorrelation of the onset strength
    double echo_ms = 0.0;
    double echo_strength = 0.0;   // normalised autocorrelation at that lag, 0..1
    bool echo_level_found = false;
    double delay_wet_db = 0.0;    // first repeat under its source, clamped to [-40, 0]

    bool tempo_found = false;     // onset-strength autocorrelation, 75..160 bpm
    double tempo_bpm = 0.0;
};

// Interleaved float frames. False, with `error`, when the audio is too short
// (under 1 s), silent, or the rate unusable.
bool measure_space(const float* interleaved, std::size_t frames, unsigned channels,
                   double sample_rate, SpaceMeasure& out, std::string& error);

// WAV or BWF through the engine's reader. FLAC is not read by the engine yet:
// convert it to WAV (flac -d) first; the error says so. At most 15 minutes.
bool measure_space_file(const std::string& path, SpaceMeasure& out, std::string& error);

// `base` with every found estimate laid over it (decay clamped to 0.2..8 s).
ReverbDelayReference to_reference(const SpaceMeasure& m, ReverbDelayReference base = {});

// One line per estimate, with what was not found and why.
std::string describe_space(const SpaceMeasure& m);

}  // namespace sonora
