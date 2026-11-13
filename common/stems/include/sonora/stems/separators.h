#pragma once
#include <memory>
#include <vector>

#include "sonora/stems/stem_separator.h"

namespace sonora::stems {

// Separators available in this build, best first. The classical baseline
// (ADR-0165) registers here when it lands; the contract ships it empty.
std::vector<std::unique_ptr<StemSeparator>> default_separators();

}  // namespace sonora::stems
