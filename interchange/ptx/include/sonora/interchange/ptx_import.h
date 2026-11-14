// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Read-only .ptx import (ADR-0156). There is no .ptx export.

#include <string>

#include "sonora/interchange/interchange.h"
#include "sonora/interchange/ptx_document.h"

namespace sonora::interchange {

class PtxImporter : public Importer {
public:
    // A null reader means this build cannot decode .ptx: can_read is false.
    explicit PtxImporter(PtxReader* reader) : reader_(reader) {}

    bool can_read(const std::string& path) const override {
        if (!reader_ || path.size() < 4) return false;
        const std::string ext = path.substr(path.size() - 4);
        return ext == ".ptx" || ext == ".PTX";
    }

    // Decodes, then maps the document (ptx_map.h); unsupported features
    // reach report.dropped from the mapper.
    bool read(const std::string& path, model::Snapshot& snap, ConversionReport& report,
              std::string& error) override;

private:
    PtxReader* reader_;
};

}  // namespace sonora::interchange
