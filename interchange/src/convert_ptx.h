// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include "sonora/interchange/interchange.h"
#include "sonora/interchange/ptx_document.h"

namespace sonora::interchange {

// .ptx to a session at `dest`; media copied into <dest folder>/Audio Files/.
bool ptx_import(const std::string& in, const std::string& dest, model::Snapshot& snap,
                ConversionReport& report, std::string& error);

// Same, with the reader injected (null: this build cannot decode .ptx).
bool ptx_import_with(PtxReader* reader, const std::string& in, const std::string& dest,
                     model::Snapshot& snap, ConversionReport& report, std::string& error);

}  // namespace sonora::interchange
