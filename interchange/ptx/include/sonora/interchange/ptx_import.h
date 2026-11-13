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

    // Contract stage: the document-to-Snapshot conversion lands with the
    // follow-up; until then a decoded non-empty document reports that.
    bool read(const std::string& path, model::Snapshot&, ConversionReport& report,
              std::string& error) override {
        if (!reader_) {
            error = "no .ptx reader in this build";
            return false;
        }
        PtxDocument doc;
        if (!reader_->read(path, doc, error)) return false;
        if (doc.tracks.empty() && doc.sources.empty()) {
            error = "ptx: '" + path + "' decoded but holds no tracks or audio";
            return false;
        }
        for (const auto& u : doc.unsupported) report.dropped.push_back(u);
        error = "ptx: conversion to a session is not implemented yet";
        return false;
    }

private:
    PtxReader* reader_;
};

}  // namespace sonora::interchange
