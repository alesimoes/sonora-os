// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// PtxReader over libptformat (ADR-0156). Built only with
// SONORA_WITH_LIBPTFORMAT; the library is read-only and never written to.

#include <memory>
#include <string>

#include "sonora/interchange/interchange.h"
#include "ptx_media.h"
#include "sonora/interchange/ptx_import.h"

namespace sonora::interchange {

class LibPtformatReader : public PtxReader {
public:
    bool read(const std::string& path, PtxDocument& out, std::string& error) override;
};

// One-line wiring for sonora-convert: an importer that owns its reader.
std::unique_ptr<Importer> make_ptx_importer();

}  // namespace sonora::interchange
