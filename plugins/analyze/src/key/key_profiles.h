// SPDX-License-Identifier: GPL-3.0-or-later
//
// The 24 Krumhansl-Kessler key profiles and the correlation that ranks them
// (C. L. Krumhansl, Cognitive Foundations of Musical Pitch, Oxford, 1990,
// table 2.1: probe-tone ratings, C major and C minor). The profile of any
// other key is these rotated to its tonic.
#pragma once

#include <array>

namespace sonora::analyze::key {

constexpr double kMajorProfile[12] = {6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
constexpr double kMinorProfile[12] = {6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};

// Pearson correlation of a chroma vector (C = 0) with the profile of the key
// at `tonic`. Zero for a chroma with no variance: flat means no key, and a
// division by ~0 would turn rounding noise into a confident answer.
double profile_correlation(const std::array<double, 12>& chroma, const double* profile, int tonic) noexcept;

struct KeyRanking {
    int tonic = 0;
    bool minor = false;
    double best = 0.0;    // correlation of the winner
    double second = 0.0;  // correlation of the runner-up key
    // The share of the winner's remaining headroom (1 - second) that it takes:
    // 0 when two keys tie, 1 when the winner is perfect and the runner-up is
    // not. Normalising by the headroom keeps a flat chroma (all correlations
    // near 0) from reading as decisive.
    double confidence = 0.0;
};

KeyRanking rank_keys(const std::array<double, 12>& chroma) noexcept;

}  // namespace sonora::analyze::key
