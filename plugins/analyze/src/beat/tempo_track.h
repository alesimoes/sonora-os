// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tempo and beat phase from the onset envelope (ADR-0114).
//   Tempo: autocorrelation of the mean-removed envelope, lags for 60..200 bpm.
//   The fundamental is the shortest-lag local maximum within 90 % of the
//   strongest, so a train at 174 bpm is not halved and one at 60 is not read
//   as 120's subharmonic; a pick above 160 bpm folds to half only when the
//   half-tempo peak is clearly (15 %) stronger: the octave resolves toward
//   80..160 on evidence, never on a prior alone.
//   Phase: dynamic programming over frames, D. P. W. Ellis, "Beat tracking by
//   dynamic programming", J. New Music Research 36(1), 2007: each beat scores
//   its envelope plus the best predecessor less lambda * log^2(interval/T).
#pragma once

#include <vector>

namespace sonora::analyze::beat {

struct TempoEstimate {
    bool found = false;
    double bpm = 0.0;
    double strength = 0.0;  // normalised autocorrelation at the period, 0..1
};

TempoEstimate estimate_tempo(const std::vector<float>& onset);

// Beat frames (increasing) following period `bpm` through the envelope.
std::vector<int> track_beats(const std::vector<float>& onset, double bpm);

// Mean of min(1, envelope peak within +-2 frames) over the beats: the
// tracker's score per beat, 0..1.
double beat_strength(const std::vector<float>& onset, const std::vector<int>& beats);

// True when the local beat period (median of 5 intervals) varies by 4 % or
// less of its median. Positions in any unit.
bool tempo_is_stable(const std::vector<double>& beat_positions);

// Least-squares period of the positions, counting a missed beat as its
// intervals; 0 for fewer than two.
double fitted_period(const std::vector<double>& beat_positions);

}  // namespace sonora::analyze::beat
