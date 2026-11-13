// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/interchange/aaf_export.h"

#include "sonora/model/model.h"

namespace sonora::interchange {

bool AafExporter::write(const model::Snapshot& s, const std::string&, ConversionReport&,
                        std::string& error) {
    if (!writer_) {
        error = "AAF export unavailable: no AAF writer (built without libaaf)";
        return false;
    }
    if (!s.edit || s.edit->tracks.empty()) {
        error = "nothing to export: the session has no tracks";
        return false;
    }
    // The Snapshot to AafDocument mapping lands in the mapper slice (ADR-0161).
    error = "AAF export mapping is not implemented yet";
    return false;
}

}  // namespace sonora::interchange
