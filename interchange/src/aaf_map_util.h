// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Pure helpers of the AAF mapper (ADR-0154): text and identifier hygiene.

#include <cstdint>
#include <string>

#include "sonora/interchange/aaf_document.h"

namespace sonora::interchange::aaf_detail {

bool valid_utf8(const std::string& s);
// 64 lower-case hex digits derived from `seed`, the same every run.
std::string generated_umid(const std::string& seed);
// The 64 hex digits of a MobID, or empty when it is not a UMID.
std::string umid_from(const std::string& mob_id);
// A relative, colon-free, valid UTF-8 path.
bool path_ok(const std::string& p);
// Length in edit units of a component that has one, else 0.
std::int64_t length_of(const AafComponent& c);

}  // namespace sonora::interchange::aaf_detail
