// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/stretch/warp_map.h"

#include <algorithm>
#include <utility>

namespace sonora::stretch {

bool WarpMap::set(std::int64_t source_length, std::vector<Marker> markers, std::string& why) {
    if (source_length < 1) {
        why = "the region is empty";
        return false;
    }
    Marker prev{0, 0};
    for (std::size_t i = 0; i < markers.size(); ++i) {
        const Marker& m = markers[i];
        const std::string at = "marker " + std::to_string(i) + ": ";
        if (m.source <= prev.source || m.target <= prev.target) {
            why = at + "source and target must be > 0 and strictly increase";
            return false;
        }
        if (m.source >= source_length) {
            why = at + "source is outside the region";
            return false;
        }
        prev = m;
    }
    source_length_ = source_length;
    markers_ = std::move(markers);
    return true;
}

std::int64_t WarpMap::output_length() const {
    if (markers_.empty()) return source_length_;
    const Marker& l = markers_.back();
    return l.target + (source_length_ - l.source);
}

double WarpMap::source_at(double dst) const {
    dst = std::clamp(dst, 0.0, static_cast<double>(output_length()));
    Marker a{0, 0};
    for (const Marker& b : markers_) {
        if (dst <= static_cast<double>(b.target)) {
            const double t = (dst - static_cast<double>(a.target)) /
                             static_cast<double>(b.target - a.target);
            return static_cast<double>(a.source) + t * static_cast<double>(b.source - a.source);
        }
        a = b;
    }
    return static_cast<double>(a.source) + (dst - static_cast<double>(a.target));
}

}  // namespace sonora::stretch
