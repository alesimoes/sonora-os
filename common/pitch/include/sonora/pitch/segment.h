// SPDX-License-Identifier: GPL-3.0-or-later
//
// Note segmentation (time.pitch, ADR-0143 section 3): a contour becomes a list
// of notes, each with its span, median pitch, drift and vibrato depth.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sonora/pitch/tracker.h"

namespace sonora::pitch {

struct Note {
    // Samples of the tracked input. Notes never overlap; one that follows
    // another without a gap starts where the other ends.
    std::size_t start = 0;
    std::size_t end = 0;
    double median_cents = 0.0;
    // Change in cents across the note, from a straight-line fit.
    double drift_cents = 0.0;
    // Half the 5th-to-95th percentile spread of the contour left once the
    // straight line is removed: the amplitude of a vibrato.
    double vibrato_depth_cents = 0.0;
    // The contour the figures come from: one value per hop, unvoiced gaps
    // filled by straight lines, frame 0 centred on start + hop / 2.
    std::size_t hop = 0;
    uint32_t rate = 0;
    std::vector<float> contour_cents;
};

struct SegmentConfig {
    double min_note_ms = 60.0;      // shorter pieces are dropped
    double max_gap_ms = 30.0;       // unvoiced gap bridged inside a note
    double split_cents = 70.0;      // deviation from the note so far that starts a new one
    double split_confirm_ms = 25.0; // ... held this long
    double min_voiced_ms = 100.0;   // less voiced audio than this is refused
};

// False, with `why` for the operator, when the audio is not monophonic pitched
// material: too few voiced frames, or no note long enough. `notes` is then
// left empty.
bool segment_notes(const Contour& contour, const SegmentConfig& config, std::vector<Note>& notes,
                   std::string& why);

inline bool segment_notes(const Contour& contour, std::vector<Note>& notes, std::string& why) {
    return segment_notes(contour, SegmentConfig{}, notes, why);
}

}  // namespace sonora::pitch
