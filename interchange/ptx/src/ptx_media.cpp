// SPDX-License-Identifier: GPL-3.0-or-later
#include "ptx_media.h"

#include <filesystem>

#include "sonora/audio_import.h"

namespace sonora::interchange {

namespace fs = std::filesystem;

bool copy_ptx_media(PtxDocument& doc, const std::string& session_dir, const std::string& dest_dir,
                    std::string& error) {
    std::error_code ec;
    fs::create_directories(dest_dir, ec);
    if (ec) {
        error = "ptx: cannot create '" + dest_dir + "': " + ec.message();
        return false;
    }
    for (auto& s : doc.sources) {
        const fs::path stored(s.path);
        const fs::path name = stored.filename();
        const fs::path tries[] = {stored, fs::path(session_dir) / stored,
                                  fs::path(session_dir) / "Audio Files" / name};
        fs::path found;
        for (const auto& t : tries) {
            if (fs::is_regular_file(t, ec)) {
                found = t;
                break;
            }
        }
        if (found.empty()) {
            error = "ptx: missing audio file '" + s.path + "'";
            return false;
        }
        AudioInfo info;
        std::string why;
        if (!probe_audio(found.string(), info, why)) {
            error = "ptx: audio file '" + found.string() + "': " + why;
            return false;
        }
        const fs::path dest = fs::path(dest_dir) / name;
        fs::copy_file(found, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "ptx: cannot copy '" + found.string() + "': " + ec.message();
            return false;
        }
        s.path = dest.string();
    }
    return true;
}

}  // namespace sonora::interchange
