// SPDX-License-Identifier: GPL-3.0-or-later
//
// The classical beat pass (ADR-0114):
//
//   mono mix -> 11025 Hz (the key pass's decimator) -> Hann STFT (512
//   window, 128 hop) -> log spectral flux -> adaptive threshold -> peaks
//   (onsets) -> tempo by autocorrelation (60..200 bpm) -> beats by dynamic
//   programming -> global bpm by a least-squares fit of the beat times.
//
// An onset's position is refined in the time domain: the flux peak says
// which frame, the first sample of that window above a quarter of its
// maximum says where, so a position is good to about a millisecond rather
// than to the 11.6 ms hop. A beat with no onset near it is placed by the
// median offset between frames and refined onsets.
//
// Confidence is the tracker's score: the mean strength of the envelope at
// the beats times the autocorrelation at the period (saturating at 0.4, a
// strong figure for real material). Under kBeatMinConfidence the beats are
// not reported: a tempo that cannot be trusted is not a grid.
// Stability: the local beat period varies by more than 4 %.
//
// Offline: allocates, reads blocks, polls `run.cancel` between blocks.
#pragma once

#include "sonora/analyze/beat_detector.h"

namespace sonora::analyze {

class ClassicalBeatDetector final : public BeatDetector {
public:
    static constexpr double kMinSeconds = 4.0;
    static constexpr double kSilenceAmplitude = 1e-4;  // -80 dBFS peak

    bool detect(AudioSource& source, const RunOptions& run, BeatResult& out, std::string& error) override;
};

}  // namespace sonora::analyze
