// SPDX-License-Identifier: GPL-3.0-or-later
//
// Time compression/expansion as a ratio (ADR-0175): every way the operator
// can state the target (a length, bars, a tempo, a percent) reduces to the
// one number `elastic ratio` takes, new length / old length. Pure: no engine.
#pragma once

#include <cstdint>
#include <string>

namespace sonora::ui::tce {

struct RatioResult {
    bool ok = false;
    double ratio = 1.0;
    std::string why;  // set when !ok
};

RatioResult from_frames(int64_t source_frames, int64_t target_frames);
RatioResult from_seconds(int64_t source_frames, int sample_rate, double seconds);
// Bars of `beats_per_bar` beats at `bpm`; the target length is the whole span.
RatioResult from_bars(int64_t source_frames, int sample_rate, double bpm, int beats_per_bar,
                      double bars);
// Material recorded at source_bpm made to play at target_bpm: faster is shorter.
RatioResult from_tempo(double source_bpm, double target_bpm);
// Target length as a percent of the source (50 = half as long).
RatioResult from_percent(double percent);

// Length the ratio gives, rounded to a frame.
int64_t resulting_length(int64_t source_frames, double ratio);

}  // namespace sonora::ui::tce
