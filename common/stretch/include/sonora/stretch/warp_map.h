// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace sonora::stretch {

// Frames counted from the region's first frame, as in contracts/api/elastic.h.
struct Marker {
    std::int64_t source = 0;
    std::int64_t target = 0;
    bool operator==(const Marker& o) const { return source == o.source && target == o.target; }
};

// Source -> destination time map of one region. The start is the implicit
// marker 0:0; between markers the map is linear; what follows the last
// marker keeps its speed (slope 1).
class WarpMap {
public:
    // False, with the reason, when source_length < 1, a marker is not > 0 on
    // either side, either side does not strictly increase, or a source is
    // not inside the region. The map is then left unchanged.
    bool set(std::int64_t source_length, std::vector<Marker> markers, std::string& why);

    std::int64_t source_length() const { return source_length_; }
    const std::vector<Marker>& markers() const { return markers_; }
    // Frames of the rendered result: the last marker's target plus the
    // source that follows it.
    std::int64_t output_length() const;
    // Source position (fractional) that lands at output frame `dst`, in
    // [0, source_length]; dst is clamped to [0, output_length()].
    double source_at(double dst) const;

private:
    std::int64_t source_length_ = 0;
    std::vector<Marker> markers_;
};

}  // namespace sonora::stretch
