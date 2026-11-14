// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include "sonora/interchange/ptx_document.h"

namespace sonora::interchange {

// Copies every source of `doc` into `dest_dir` (created if absent) and
// rewrites its path to the copy. A source is looked up as stored, then under
// `session_dir` and `session_dir/Audio Files`. A missing or unreadable file
// is an error naming it; nothing is skipped silently.
bool copy_ptx_media(PtxDocument& doc, const std::string& session_dir, const std::string& dest_dir,
                    std::string& error);

}  // namespace sonora::interchange
