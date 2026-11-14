// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Exact edit-unit conversion for the AAF mapper (ADR-0154): video-rate edit
// rates (25/1, 30000/1001) go through integer rationals, never a double, so a
// position that lands on a whole sample is exact and one that does not is
// reported as rounded.

#include <cstdint>

#include "sonora/interchange/aaf_document.h"

namespace sonora::interchange {

// `units` edit units at `from` units per second, as samples at `rate` Hz:
// units * rate * from.den / from.num, rounded half away from zero. `exact` is
// false when it was rounded. False (out untouched) when `from` is not a
// positive rate or the product does not fit in 64 bits.
bool rescale_units(std::int64_t units, const AafRational& from, std::int64_t rate,
                   std::int64_t& out, bool& exact);

}  // namespace sonora::interchange
