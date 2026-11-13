// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/interchange/aaf_import.h"

namespace sonora::interchange {

bool AafImporter::can_read(const std::string& path) const {
    if (!reader_) return false;
    return path.size() > 4 && path.compare(path.size() - 4, 4, ".aaf") == 0;
}

bool AafImporter::read(const std::string& path, model::Snapshot&, ConversionReport&,
                       std::string& error) {
    if (!reader_) {
        error = "AAF import is not available: no reader";
        return false;
    }
    AafDocument doc;
    if (!reader_->read(path, doc, error)) return false;
    if (doc.mobs.empty()) {
        error = "AAF file holds no mobs: nothing to import";
        return false;
    }
    error = "AAF mapping is not implemented yet";
    return false;
}

}  // namespace sonora::interchange
