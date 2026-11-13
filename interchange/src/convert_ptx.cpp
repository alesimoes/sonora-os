// SPDX-License-Identifier: GPL-3.0-or-later
#include "convert_ptx.h"

#include <filesystem>
#include <memory>

#include "ptx_map.h"
#include "ptx_media.h"
#ifdef SONORA_WITH_LIBPTFORMAT
#include "ptx_libptformat_reader.h"
#endif

namespace sonora::interchange {

namespace fs = std::filesystem;

bool ptx_import_with(PtxReader* reader, const std::string& in, const std::string& dest,
                     model::Snapshot& snap, ConversionReport& report, std::string& error) {
    if (!reader) {
        error = "no .ptx reader in this build (CMake option SONORA_WITH_LIBPTFORMAT is off)";
        return false;
    }
    PtxDocument doc;
    if (!reader->read(in, doc, error)) return false;
    if (doc.tracks.empty() && doc.sources.empty()) {
        error = "ptx: '" + in + "' decoded but holds no tracks or audio";
        return false;
    }
    const fs::path folder = fs::absolute(fs::path(dest)).parent_path();
    const fs::path session_dir = fs::absolute(fs::path(in)).parent_path();
    if (!copy_ptx_media(doc, session_dir.string(), (folder / "Audio Files").string(), error))
        return false;
    return map_ptx_document(doc, 0, snap, report, error) &&
           write_session(dest, snap, report, error);
}

bool ptx_import(const std::string& in, const std::string& dest, model::Snapshot& snap,
                ConversionReport& report, std::string& error) {
#ifdef SONORA_WITH_LIBPTFORMAT
    LibPtformatReader reader;
    return ptx_import_with(&reader, in, dest, snap, report, error);
#else
    return ptx_import_with(nullptr, in, dest, snap, report, error);
#endif
}

}  // namespace sonora::interchange
