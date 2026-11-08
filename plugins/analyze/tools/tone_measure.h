// SPDX-License-Identifier: GPL-3.0-or-later
//
// The tone fingerprint of an audio source and the fixed stimulus a capture
// is rendered with (ADR-0072 §2, §3). Shared by `sonora-analyze tone`
// (the reference) and ampcab-tone-profile (the captures), so both sides of
// a comparison go through the same code. Offline only.
#pragma once

#include <string>
#include <vector>

#include "sonora/analyze/mix_analysis.h"
#include "sonora/analyze/tone_profile.h"

namespace sonora::analyze {

// kToneStimulusId's signal: ten seconds of pink noise at -20 dBFS RMS,
// 48 kHz, from a fixed-seed integer generator and Kellett's pink filter, so
// it is the same on every machine and nothing third-party is in it. Pink
// because a played guitar's long-term spectrum is broadband and falls with
// frequency; the noise exercises every band at a level the timbre pass's
// silence gate accepts. Whether it is representative of a played DI is
// ADR-0072 Unverified 2.
constexpr double kToneStimulusRate = 48000.0;
std::vector<float> tone_stimulus();

// Bands (relative to the mean of the valid bands), peak-to-RMS crest over
// all channels and the flatness at and above 2 kHz. False with `error`
// when the source cannot be read or the run is cancelled ("cancelled").
bool measure_tone_profile(AudioSource& source, const RunOptions& run, ToneProfile& out, std::string& error);

}  // namespace sonora::analyze
