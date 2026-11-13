// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/interchange/ptx_import.h"

#include "ptx_map.h"

namespace sonora::interchange {

bool PtxImporter::read(const std::string& path, model::Snapshot& snap, ConversionReport& report,
                       std::string& error) {
    if (!reader_) {
        error = "no .ptx reader in this build";
        return false;
    }
    PtxDocument doc;
    if (!reader_->read(path, doc, error)) return false;
    if (doc.tracks.empty() && doc.sources.empty()) {
        error = "ptx: '" + path + "' decoded but holds no tracks or audio";
        return false;
    }
    return map_ptx_document(doc, 0, snap, report, error);
}

}  // namespace sonora::interchange
