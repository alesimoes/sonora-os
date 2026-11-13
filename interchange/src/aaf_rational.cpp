// SPDX-License-Identifier: GPL-3.0-or-later
#include "aaf_rational.h"

#include <limits>
#include <numeric>

namespace sonora::interchange {

bool rescale_units(std::int64_t units, const AafRational& from, std::int64_t rate,
                   std::int64_t& out, bool& exact) {
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    if (from.num <= 0 || from.den <= 0 || rate <= 0) return false;
    if (from.den > kMax / rate) return false;
    std::int64_t p = rate * from.den;
    std::int64_t q = from.num;
    const std::int64_t g = std::gcd(p, q);
    p /= g;
    q /= g;
    const bool negative = units < 0;
    // -INT64_MIN does not exist; no timeline value is that large anyway.
    if (units == std::numeric_limits<std::int64_t>::min()) return false;
    const std::int64_t mag = negative ? -units : units;
    if (mag > kMax / p) return false;
    const std::int64_t n = mag * p;
    std::int64_t r = n / q;
    const std::int64_t rem = n % q;
    exact = rem == 0;
    // rem < q <= INT64_MAX, so 2 * rem can overflow; compare against the rest.
    if (rem != 0 && rem >= q - rem) ++r;
    out = negative ? -r : r;
    return true;
}

}  // namespace sonora::interchange
