// SPDX-License-Identifier: GPL-3.0-or-later
//
// editor.h in a build without X11 (no libx11-dev at configure time): every
// OpenEditor is answered EditorFailed with this reason.

#include "editor.h"

namespace sonora::plugin_host {

std::unique_ptr<EditorDisplay> EditorDisplay::open(std::string& error) {
    error = "this build of sonora-plugin-host has no X11, so it opens no plugin editor";
    return nullptr;
}

}  // namespace sonora::plugin_host
