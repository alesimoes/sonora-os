// SPDX-License-Identifier: GPL-3.0-or-later
//
// Session templates (ADR-0030): a template is a session file of the current
// format whose document holds a session's setup and none of its material.
// It has no key or version of its own; what makes it a template is the
// folder it is kept in. No I/O here: the bytes are model::serialize()'s and
// the folder is written by the engine's saver, as for Save Copy In.
#pragma once

#include <string>

#include "sonora/model/model.h"

namespace sonora::model {

struct TemplateOptions {
    // Pro Tools leaves the choice to the operator: a song form is worth
    // keeping, a session's cue points usually are not.
    bool markers = false;
};

// What Save as Template writes (ADR-0030 §2). Kept: the tracks in their
// order with their kinds, channels, inserts, sends, outputs and automation;
// the buses; the whole mix (faders, pans, inputs, monitor modes, MIDI
// routing, every insert's values and state reference, every send's and
// bus's values); the tempo map, timecode, transport and settings; the
// markers when `options.markers`. Dropped: every region and crossfade,
// every playlist but each track's active one (kept empty, with its id and
// name), every source, the left takes and the session id. Revision 0;
// next_id as the session's, so no id is ever reused against it.
Snapshot template_of(const Snapshot& session, const TemplateOptions& options = {});

// New from Template (ADR-0030 §3): `tmpl` with its markers, named `name`,
// with `session_id` (a fresh new_session_id()). Material in `tmpl` is
// dropped as template_of() drops it, so a session file put in the
// templates folder by hand still starts a session with no take of another
// session's in it.
Snapshot session_from_template(const Snapshot& tmpl, const std::string& name,
                               const std::string& session_id);

}  // namespace sonora::model
