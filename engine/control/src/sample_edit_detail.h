// SPDX-License-Identifier: GPL-3.0-or-later
// Shared between the sample_edit*.cpp files only; not part of the module's API.
#pragma once

#include <string>

#include "sonora/sample_edit.h"

namespace sonora {
namespace sample_edit_detail {

const char* label_of(SampleVerb v);

// The pitch verb's own checks on a parsed request and its PARAM.
bool parse_pitch_args(const std::string& param, SampleEditRequest& out, std::string& why);

// Renders the pitch verb whole, through engine/pitch.
bool pitch_edit(model::Document& document, const Session& session, const std::string& session_dir,
                const SampleEditRequest& rq, std::vector<model::ReplicationUpdate>& updates,
                std::string& error);

}  // namespace sample_edit_detail
}  // namespace sonora
