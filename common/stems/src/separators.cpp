#include "sonora/stems/separators.h"

namespace sonora::stems {

std::vector<std::unique_ptr<StemSeparator>> default_separators() {
    std::vector<std::unique_ptr<StemSeparator>> v;
    v.push_back(std::make_unique<HpssSeparator>());
    v.push_back(std::make_unique<CentreSeparator>());
    return v;
}

}  // namespace sonora::stems
