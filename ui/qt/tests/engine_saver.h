// SPDX-License-Identifier: GPL-3.0-or-later
//
// The engine's saver standing in behind a ModelDocument, for the tests that
// save without an engine: the same common/session_files code the engine's
// saver thread runs, unpaced (no take is open here), cleaning up after a
// durable save as the engine does. The interface itself does not link it.
#pragma once

#include <memory>
#include <string>

#include "sonora/session_files/session_folder.h"
#include "timeline_document.h"

namespace sonora::ui::testing {

inline ModelDocument::Saver engine_saver() {
    auto folder = std::make_shared<session_files::SessionFolder>();
    return [folder](const std::string& file, const std::string& bytes,
                    const model::Snapshot& document, std::string& why, std::string& caution) {
        if (!folder->save(file, bytes, document, why, caution)) return false;
        if (caution.empty()) folder->clean(caution);
        return true;
    };
}

}  // namespace sonora::ui::testing
