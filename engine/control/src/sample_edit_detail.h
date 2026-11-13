// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the files of sample.edit share and no caller outside them needs:
// sample_edit_parse.cpp reads the command, sample_edit_pitch.cpp hands the
// pitch verb to engine/pitch, sample_edit.cpp does the other verbs.
#pragma once

#include <string>
#include <vector>

#include "sonora/sample_edit.h"

namespace sonora::sample_edit_detail {

// The verb as the operator reads it, for the undo label and the file's
// description: "Fade In", "Pitch".
const char* verb_label(SampleVerb verb);

// The PARAM of a pitch verb into out.pitch; out.channel is already read.
bool parse_pitch_verb(const std::string& param, SampleEditRequest& out, std::string& why);

// sample_edit() for the pitch verb: engine/pitch renders it.
bool pitch_edit(model::Document& document, const Session& session, const std::string& session_dir,
                const SampleEditRequest& request, std::vector<model::ReplicationUpdate>& updates,
                std::string& error);

}  // namespace sonora::sample_edit_detail
