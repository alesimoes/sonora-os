// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cmath>

#include "sonora/stretch/stretcher.h"

namespace sonora::stretch {

bool LinearResampleStretcher::process(const Planar& input, const WarpMap& map, Algorithm,
                                      Planar& output, std::string& why) {
    if (input.empty() || input[0].empty()) {
        why = "no audio";
        return false;
    }
    const std::size_t n = input[0].size();
    for (const auto& ch : input) {
        if (ch.size() != n) {
            why = "channels differ in length";
            return false;
        }
    }
    if (static_cast<std::int64_t>(n) != map.source_length()) {
        why = "input length differs from the map's source length";
        return false;
    }
    const std::int64_t out_n = map.output_length();
    output.assign(input.size(), std::vector<float>(static_cast<std::size_t>(out_n), 0.0f));
    for (std::int64_t i = 0; i < out_n; ++i) {
        const double s = map.source_at(static_cast<double>(i));
        const auto i0 = std::min<std::size_t>(static_cast<std::size_t>(std::floor(s)), n - 1);
        const auto i1 = std::min(i0 + 1, n - 1);
        const double f = s - static_cast<double>(i0);
        for (std::size_t c = 0; c < input.size(); ++c) {
            output[c][static_cast<std::size_t>(i)] =
                static_cast<float>(input[c][i0] * (1.0 - f) + input[c][i1] * f);
        }
    }
    return true;
}

}  // namespace sonora::stretch
