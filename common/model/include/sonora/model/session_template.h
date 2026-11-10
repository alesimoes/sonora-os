// SPDX-License-Identifier: GPL-3.0-or-later
//
// Session templates (ADR-0030): a template is a session file of the current
// format whose document holds a session's setup and none of its material.
// It has no key or version of its own; what makes it a template is the
// folder it is kept in. No I/O here: the bytes are model::serialize()'s and
// the folder is written by the engine's saver, as for Save Copy In.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "sonora/model/model.h"
#include "sonora/model/session_file.h"

namespace sonora::model {

// One template as a template browser lists it (ADR-0146). Filled by whoever
// lists the templates folder; the model does no I/O. Every field has a
// default so a caller that sets only name and file still compiles.
struct TemplateInfo {
    std::string name;  // the template folder's name
    std::string file;  // its .sonora file
    // The one subfolder of the templates folder it sits in. Empty is
    // uncategorised: a template directly under the templates folder.
    std::string category;
    // What the file declares, read by file_format_version() and never by a
    // full load; 0 until read.
    std::int64_t file_format_version = 0;
    // Declared above kFormatVersion: listed greyed, never opened, since the
    // loader would refuse it anyway (ADR-0030 §5).
    bool newer_than_build = false;
};

inline bool format_newer_than_build(std::int64_t version) { return version > kFormatVersion; }

// Sets `info`'s version fields from the file's bytes without deserializing
// them. False, leaving `info` as it was, when the bytes are not a session
// file: the lister leaves such a folder out, as list_sessions() does.
inline bool read_template_version(std::string_view bytes, TemplateInfo& info) {
    std::int64_t version = 0;
    if (!file_format_version(bytes, version)) return false;
    info.file_format_version = version;
    info.newer_than_build = format_newer_than_build(version);
    return true;
}

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
