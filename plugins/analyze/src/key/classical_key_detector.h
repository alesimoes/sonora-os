// SPDX-License-Identifier: GPL-3.0-or-later
//
// The classical key pass (ADR-0066 §1): chroma and Krumhansl-Schmuckler.
//
//   mono mix -> 11025 Hz -> Hann STFT (8192 window, 2048 hop) -> bins from
//   65 Hz to 2 kHz folded to a chroma -> long-term chroma, each frame
//   weighted by its RMS, frames under -60 dBFS left out -> correlation with
//   the 24 Krumhansl-Kessler profiles (key_profiles.h).
//
// Tuning: a recording is rarely at A = 440 Hz. Spectral peaks (parabolic
// interpolation of the log magnitude) give each partial's offset from the
// nearest equal-tempered note; their magnitude-weighted circular mean over
// 100 cents is the tuning offset, within +-50 cents. The chroma folds on a
// 5-cent grid and the semitone cells are then centred on the estimate, so a
// detuned recording does not smear across two pitch classes.
//
// Offline: allocates, reads blocks, polls `run.cancel` once per hop.
#pragma once

#include "sonora/analyze/key_detector.h"

namespace sonora::analyze {

class ClassicalKeyDetector final : public KeyDetector {
public:
    static constexpr double kAnalysisRate = 11025.0;
    static constexpr int kWindow = 8192;
    static constexpr int kHop = 2048;
    static constexpr double kMinHz = 65.0;
    static constexpr double kMaxHz = 2000.0;
    static constexpr double kGateDbfs = -60.0;

    bool detect(AudioSource& source, const RunOptions& run, KeyResult& out, std::string& error) override;

    // The tuning offset of the last successful detect(), in cents.
    double tuning_cents() const noexcept { return tuning_cents_; }

private:
    double tuning_cents_ = 0.0;
};

}  // namespace sonora::analyze
