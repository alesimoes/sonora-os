// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fakes for the beat pass (ADR-0114), so the command's, the tempo
// function's and the interface's tests run without audio: a detector that
// returns a scripted result or error, and a builder for an even beat grid.
#pragma once

#include <cstdint>
#include <string>

#include "sonora/analyze/beat_detector.h"

namespace sonora::analyze {

// A steady `bpm` from frame `first` to the end of `seconds` of audio, one
// onset on every beat, stable, at `confidence`.
inline BeatResult fake_beat_result(double bpm = 120.0, double confidence = 0.9, std::uint32_t sample_rate = 48000,
                                   double seconds = 8.0, std::int64_t first = 0) {
    BeatResult r;
    r.sample_rate = sample_rate;
    r.frames = static_cast<std::int64_t>(seconds * sample_rate);
    const double period = 60.0 * sample_rate / bpm;
    for (int i = 0;; ++i) {
        const std::int64_t at = first + static_cast<std::int64_t>(i * period + 0.5);
        if (at >= r.frames) break;
        r.beats.push_back(at);
    }
    r.onsets = r.beats;
    r.bpm = bpm;
    r.confidence = confidence;
    r.stable = true;
    return r;
}

class FakeBeatDetector final : public BeatDetector {
public:
    BeatResult result = fake_beat_result();
    std::string fail_with;  // non-empty: detect fails with this error
    int calls = 0;
    std::int64_t last_frames = -1;

    bool detect(AudioSource& source, const RunOptions& run, BeatResult& out, std::string& error) override {
        ++calls;
        last_frames = source.frames();
        if (run.cancel && run.cancel->load()) {
            error = "cancelled";
            return false;
        }
        if (!fail_with.empty()) {
            error = fail_with;
            return false;
        }
        out = result;
        return true;
    }
};

}  // namespace sonora::analyze
