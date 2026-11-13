// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include "sonora/interchange/aaf_document.h"
#include "sonora/interchange/interchange.h"

namespace sonora::interchange {

// Serialises a neutral AafDocument to an .aaf file (ADR-0161). The only
// implementation that sees libaaf types lives behind this seam.
class AafWriter {
public:
    virtual ~AafWriter() = default;
    virtual bool write(const AafDocument&, const std::string& path, std::string& error) = 0;
};

// Native session to AAF. The writer is injected; with null, write fails with
// a clear error rather than producing nothing.
class AafExporter : public Exporter {
public:
    explicit AafExporter(AafWriter* writer) : writer_(writer) {}
    bool write(const model::Snapshot&, const std::string& path, ConversionReport&,
               std::string& error) override;

private:
    AafWriter* writer_;
};

}  // namespace sonora::interchange
