// SPDX-License-Identifier: GPL-3.0-or-later
//
// Arrangement sections (time.arrangement, ADR-0116): a section is a range
// marker, and moving or copying one is a plan of existing operations in one
// Compound, so it is one undo step and needs no new operation, protocol or
// session format.
//
// Both modes insert: material at or after the destination moves later by the
// section's length, and the section's audio fills the gap; nothing is
// overwritten. Move then closes the gap the section left. Off the audio
// thread: it allocates, and it applies each member to a working copy so a
// plan it returns is one apply() accepts.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sonora/model/model.h"
#include "sonora/model/operations.h"

namespace sonora::model {

enum class SectionMode : std::uint8_t { Copy, Move };

struct SectionEdit {
    Id section = 0;          // a marker with length > 0
    Frames destination = 0;  // timeline frame, before the edit
    SectionMode mode = SectionMode::Copy;
};

// Either a compound or a refusal: `refusal` empty means `compound` holds
// the plan, else it says why nothing can be applied.
struct SectionPlan {
    Compound compound;
    std::string refusal;
    bool ok() const { return refusal.empty(); }
};

// What a section edit leaves where it was (ADR-0116 Decision 5), for the
// interface to show with the result.
extern const char* const kSectionEditLimits;

// The sections of `s`: its range markers, in marker order.
std::vector<Marker> section_markers(const Snapshot& s);

// The plan for `edit` on `s`, taking ids from `next_id` (at least
// s.next_id) strictly upward in member order. Only the active playlist of
// each audio track changes; MIDI tracks are skipped. A Move to a
// destination after the section lands it at destination - length, before
// the material that was at the destination.
//
// Refused: a destination inside the section (or, for Move, at either of its
// ends, where it already is), before frame 0 or pushing material past the
// timeline; a section with no audio and no markers in it; a plan longer
// than kMaxCompoundMembers (the message names its length).
SectionPlan plan_section_edit(const Snapshot& s, const SectionEdit& edit, Id next_id);

}  // namespace sonora::model
