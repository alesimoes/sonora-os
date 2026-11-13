// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>

#include "sonora/interchange/interchange.h"
#include "sonora/interchange/omf_document.h"

namespace sonora::interchange {

// OMF to a native session (ADR-0160), read-only. The reader is injected; with
// none, can_read is false.
class OmfImporter : public Importer {
public:
    explicit OmfImporter(OmfReader* reader) : reader_(reader) {}
    bool can_read(const std::string& path) const override;
    bool read(const std::string& path, model::Snapshot& out, ConversionReport&,
              std::string& error) override;

private:
    OmfReader* reader_;
};

// Reads OMFI files; embedded media is extracted into media_dir (the session
// folder, which must exist).
std::unique_ptr<OmfReader> make_omf_reader(const std::string& media_dir);
// One-line wiring for sonora-convert: an importer that owns its reader.
std::unique_ptr<Importer> make_omf_importer(const std::string& media_dir);

}  // namespace sonora::interchange
