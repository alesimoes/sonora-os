// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/interchange/omf_import.h"

#include <cctype>

#include "omf_map.h"

namespace sonora::interchange {

bool OmfImporter::can_read(const std::string& path) const {
    if (!reader_) return false;
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == "omf" || ext == "omfi";
}

bool OmfImporter::read(const std::string& path, model::Snapshot& out, ConversionReport& report,
                       std::string& error) {
    if (!reader_) {
        error = "OMF import is not available in this build";
        return false;
    }
    OmfDocument doc;
    if (!reader_->read(path, doc, error)) return false;
    if (doc.tracks.empty()) {
        error = "OMF file has no tracks";
        return false;
    }
    return map_omf(doc, 48000, out, report, error);
}

}  // namespace sonora::interchange
