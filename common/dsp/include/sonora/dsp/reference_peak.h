// SPDX-License-Identifier: GPL-3.0-or-later
//
// The reference true-peak meter the limiter is judged by (ADR-0011 §3.2,
// "Ceiling": a meter oversampled more finely than the limiter's own).
// Offline only: it allocates, and it is far too slow for a callback.
//
// The reference is a 16x band-limited reconstruction: a Kaiser-windowed
// sinc of +-256 taps at 120 dB, longer than any detector of ours as well as
// finer. Evaluating it at every 16th of a sample costs 7680 products per
// sample, so it is evaluated only around the peaks:
// 1. a pre-scan upsamples 4x through the half-band cascade with its passband
//    to 0.495 fs, close to the reference's own reconstruction;
// 2. at each local maximum of the pre-scan's magnitude that reaches
//    near_level * cos(pi / 8) (a band-limited peak is at most 1 / cos(pi / 8)
//    above the nearest 4x point), less 3 % for the two filters' difference,
//    the reference is evaluated on its 16x grid over the quarter sample
//    either side, which is where that peak lies.
// A peak of the pre-scan is a peak of the reconstruction, so nothing above
// near_level is skipped; below near_level the value returned is only a
// lower bound, which is all a ceiling test needs. An earlier version
// evaluated only between samples within 6 dB of the level. That skipped
// near-Nyquist peaks whose samples are near zero, which is where full-band
// noise puts its intersample peaks.
#pragma once

#include <cstddef>

namespace sonora::dsp {

// Largest |value| of the 16x reconstruction of y[0, n), exact wherever it
// reaches near_level, and never less than the largest |sample|.
double reference_true_peak(const float* y, std::size_t n, double near_level);

}  // namespace sonora::dsp
