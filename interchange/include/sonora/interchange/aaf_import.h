// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "sonora/interchange/aaf_document.h"
#include "sonora/interchange/interchange.h"

namespace sonora::interchange {

// AAF to a native session (ADR-0154). The reader is injected; with none,
// can_read is false.
class AafImporter : public Importer {
public:
    explicit AafImporter(AafReader* reader) : reader_(reader) {}
    bool can_read(const std::string& path) const override;
    bool read(const std::string& path, model::Snapshot& out, ConversionReport&,
              std::string& error) override;

private:
    AafReader* reader_;
};

}  // namespace sonora::interchange
