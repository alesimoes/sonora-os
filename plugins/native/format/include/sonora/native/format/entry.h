// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one symbol a bundle supplies to the format glue: which Processor it
// wraps. sonora_native_plugin() (plugins/native/format/CMakeLists.txt)
// generates its definition from the processor's factory, so a processor
// never includes anything of a plugin format (ADR-0011 §1.2).
#pragma once

#include <memory>

#include "sonora/native/processor.h"

namespace sonora::native::format {

std::unique_ptr<Processor> make_bundle_processor();

}  // namespace sonora::native::format
