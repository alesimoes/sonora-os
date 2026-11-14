// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// PtxDocument to model::Snapshot (ADR-0156). Offline, sonora-convert only.

#include <cstdint>
#include <string>

#include "sonora/interchange/interchange.h"
#include "sonora/interchange/ptx_document.h"

namespace sonora::interchange {

// Builds a session of mono audio tracks, one playlist each. Anything the
// session cannot hold goes to report.dropped, any rounding to
// report.approximated. False with `error` set when the document is
// inconsistent (overlapping regions, a region naming a missing source, a
// zero or unusable sample rate). `session_rate` 0 keeps the document's rate;
// another value rescales every position, which is reported, because the
// source files are not resampled.
bool map_ptx_document(const PtxDocument& doc, std::uint32_t session_rate, model::Snapshot& out,
                      ConversionReport& report, std::string& error);

}  // namespace sonora::interchange
