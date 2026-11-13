// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
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

// An exporter owning the build's AafWriter. Without SONORA_WITH_LIBAAF (or
// with a libaaf that cannot write) its write fails loudly, never an empty file.
std::unique_ptr<Exporter> make_aaf_exporter();

}  // namespace sonora::interchange
