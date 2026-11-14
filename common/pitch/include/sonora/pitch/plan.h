// SPDX-License-Identifier: GPL-3.0-or-later
//
// The pitch plan (time.pitch, ADR-0143 section 3): the notes found in a
// region, an edit per note, and the PitchEditSpec those edits come to.
//
// A PitchEditSpec shifts a span by a constant, so straightening drift or
// scaling vibrato is done by cutting the note into short pieces, each shifted
// by the mean correction over it. Pieces are 50 ms or longer (they grow when
// the 64-note limit would otherwise be passed), so a vibrato faster than about
// 4 Hz is only partly flattened; the plan does not pretend otherwise.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sonora/api/pitch.h"
#include "sonora/pitch/segment.h"

namespace sonora::pitch {

enum class Scale { Chromatic, Major, NaturalMinor, HarmonicMinor, MajorPentatonic, MinorPentatonic };

struct NoteEdit {
    double shift_cents = 0.0;     // on top of the note's own pitch
    double drift_scale = 1.0;     // 1 keeps the drift, 0 removes it
    double vibrato_scale = 1.0;   // 1 keeps the vibrato, 0 removes it, 2 doubles it
    int32_t formant_cents = 0;
    double gain_db = 0.0;
};

class PitchPlan {
public:
    explicit PitchPlan(std::vector<Note> notes);

    std::size_t size() const { return notes_.size(); }
    const Note& note(std::size_t i) const { return notes_[i]; }
    const NoteEdit& edit(std::size_t i) const { return edits_[i]; }
    NoteEdit& edit(std::size_t i) { return edits_[i]; }

    // Moves every note's shift towards the nearest degree of the scale. `key`
    // is the tonic as a pitch class, 0 = C .. 11 = B; `amount` is 0 (no
    // change) to 100 (on the degree), in percent. The tuning follows the
    // contour's A4: cents are measured from it.
    bool snap_to_scale(int key, Scale scale, double amount, std::string& why);

    // The spec for a region `region_length` frames long: notes past its end
    // are cut or dropped, notes that change nothing are left out, and the
    // result never overlaps and never holds more than api::kMaxPitchNotes.
    // False, with `why`, when nothing would change or it cannot fit.
    bool to_spec(uint64_t region_length, api::PitchEditSpec& spec, std::string& why) const;

private:
    std::vector<Note> notes_;
    std::vector<NoteEdit> edits_;
};

}  // namespace sonora::pitch
