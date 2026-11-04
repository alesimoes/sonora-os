// SPDX-License-Identifier: GPL-3.0-or-later
//
// Strip Silence (Pro Tools' Ctrl+U, Logic's Remove Silence): where a region
// stays below a threshold long enough to be cut out. Pure: it reads the
// region's overviews through PeakSource, never the audio, and answers in
// timeline frames so EditController can turn them into separations.
#pragma once

#include <string>
#include <vector>

#include "model_access.h"
#include "peak_source.h"

namespace sonora::ui {

struct StripParams {
    double threshold_db = -40.0;    // below this peak level a stretch is quiet
    double min_silence_ms = 100.0;  // a quiet stretch shorter than this stays
    double start_pad_ms = 10.0;     // kept before the sound that follows a cut
    double end_pad_ms = 10.0;       // kept after the sound that precedes a cut
};

// "threshold_db[,min_silence_ms[,start_pad_ms[,end_pad_ms]]]"; empty keeps
// the defaults. False, and why, for anything that is not a number in range.
bool parse_strip_params(const std::string& text, StripParams& out, std::string& why);

struct SilentSpan {
    model::Frames from = 0;  // timeline frames, [from, to)
    model::Frames to = 0;
};

// The stretches of `r` to remove, ascending and apart. A region quiet from
// end to end gives one span covering it.
std::vector<SilentSpan> find_silence(const PeakSource& peaks, const model::Region& r,
                                     uint32_t sample_rate, const StripParams& p);

}  // namespace sonora::ui
