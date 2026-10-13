// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where waveform overviews come from. The timeline asks for min/max pairs
// over a run of equal buckets of a source; it never reads audio. The real
// implementation reads the peak files the engine writes beside each take
// (Phase 3, ui.editing.waveform); until then SyntheticPeakSource draws a
// deterministic envelope, and the Edit window says the session is synthetic.
#pragma once

#include <cstdint>

#include "model_access.h"

namespace sonora::ui {

struct PeakPair {
    float min = 0.0f;  // -1 .. 0
    float max = 0.0f;  //  0 .. 1
};

class PeakSource {
public:
    virtual ~PeakSource() = default;
    // `count` buckets of `frames_per_bucket` source frames each, the first
    // starting at source frame `first`. A bucket outside the source is
    // silence. Runs on the GUI thread when the timeline is rebuilt, so it
    // must answer from memory, never from disk.
    virtual void peaks(model::Id source, uint16_t channel, model::Frames first,
                       double frames_per_bucket, int count, PeakPair* out) const = 0;
};

class SyntheticPeakSource final : public PeakSource {
public:
    SyntheticPeakSource(uint32_t sample_rate, double bpm) : rate_(sample_rate), bpm_(bpm) {}
    void peaks(model::Id source, uint16_t channel, model::Frames first,
               double frames_per_bucket, int count, PeakPair* out) const override;

    // The envelope at one source frame, 0..1. Public for the tests.
    float envelope(model::Id source, model::Frames frame) const;

private:
    uint32_t rate_;
    double bpm_;
};

}  // namespace sonora::ui
