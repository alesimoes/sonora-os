// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// PtxReader over libptformat (ADR-0156). Built only with
// SONORA_WITH_LIBPTFORMAT; the library is read-only and never written to.

#include <memory>
#include <string>

#include "sonora/interchange/interchange.h"
#include "sonora/interchange/ptx_import.h"

namespace sonora::interchange {

class LibPtformatReader : public PtxReader {
public:
    bool read(const std::string& path, PtxDocument& out, std::string& error) override;
};

// One-line wiring for sonora-convert: an importer that owns its reader.
std::unique_ptr<Importer> make_ptx_importer();

// Copies every source of `doc` into `dest_dir` (created if absent) and
// rewrites its path to the copy. A source is looked up as stored, then under
// `session_dir` and `session_dir/Audio Files`. A missing or unreadable file
// is an error naming it; nothing is skipped silently.
bool copy_ptx_media(PtxDocument& doc, const std::string& session_dir, const std::string& dest_dir,
                    std::string& error);

}  // namespace sonora::interchange
