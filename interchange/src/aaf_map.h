// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// AafDocument to a native session (ADR-0154). Pure: no file, no libaaf.

#include <cstdint>
#include <string>

#include "sonora/interchange/aaf_document.h"
#include "sonora/interchange/interchange.h"

namespace sonora::interchange {

// Composition slots become audio tracks, a Sequence its regions (a Filler is a
// gap), a Transition a crossfade, a lone fade fade_in/out, clip gain gain_db.
// Positions are converted to `sample_rate` by exact rationals; every rounding
// goes in `report.approximated`, what has no place in the model in
// `report.dropped`. False with `error` set for a document that cannot be
// mapped (no composition, overlapping clips); `out` is then untouched. The
// result has passed model::check_invariants.
bool map_aaf(const AafDocument& doc, std::uint32_t sample_rate, model::Snapshot& out,
             ConversionReport& report, std::string& error);

}  // namespace sonora::interchange
