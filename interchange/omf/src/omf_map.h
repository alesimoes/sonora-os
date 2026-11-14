// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

#include "sonora/interchange/interchange.h"
#include "sonora/interchange/omf_document.h"

namespace sonora::interchange {

// OmfDocument to a native session (ADR-0160): sources, one track per OMF
// track, one region per clip with gain, mute and fades. Positions are moved
// from the document's rate to `sample_rate` and every rounding goes in
// `report.approximated`; what has no place in the model (clips on a missing
// source, the reader's `unsupported` lines) goes in `report.dropped`. False
// with `error` set when the document cannot be mapped (no tracks, overlapping
// clips on a track); `out` is then untouched. The result has passed
// model::check_invariants.
bool map_omf(const OmfDocument& doc, std::uint32_t sample_rate, model::Snapshot& out,
             ConversionReport& report, std::string& error);

}  // namespace sonora::interchange
