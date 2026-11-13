// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "sonora/stretch/warp_map.h"

namespace sonora::stretch {

enum class Algorithm : std::uint8_t { Polyphonic, Rhythmic, Monophonic, Varispeed };

using Planar = std::vector<std::vector<float>>;  // [channel][frame]

// Renders a region through a warp map. Offline, control thread only.
class Stretcher {
public:
    virtual ~Stretcher() = default;
    // Output has the input's channel count and map.output_length() frames.
    // False, with the reason, when the input is empty, its channels differ in
    // length, or its length is not map.source_length().
    virtual bool process(const Planar& input, const WarpMap& map, Algorithm algorithm,
                         Planar& output, std::string& why) = 0;
};

// Deterministic linear resampler per segment; ignores the algorithm. For tests.
class LinearResampleStretcher final : public Stretcher {
public:
    bool process(const Planar& input, const WarpMap& map, Algorithm algorithm, Planar& output,
                 std::string& why) override;
};

}  // namespace sonora::stretch
