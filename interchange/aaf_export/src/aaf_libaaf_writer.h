// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>

#include "sonora/interchange/aaf_export.h"

namespace sonora::interchange {

// The writer this build can offer; never null.
std::unique_ptr<AafWriter> make_libaaf_writer();

}  // namespace sonora::interchange
