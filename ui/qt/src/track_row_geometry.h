// SPDX-License-Identifier: GPL-3.0-or-later
//
// How the Edit view sizes a track's row (ADR-0106): the height preset scales
// the base row, and a hidden row is zero tall, so it keeps its index (every
// selection, hit test and command still counts tracks the same way) but takes
// no room and is never drawn or hit. The policy is a seam: the controller
// asks it, the interface's TrackViewModel answers, and a test hands in a fake.
#pragma once

#include <cstdint>

#include "track_views.h"

namespace sonora::ui {

struct RowSpec {
    double scale = 1.0;  // of the base row height
    bool hidden = false;
};

constexpr double height_scale(TrackHeight h) {
    switch (h) {
        case TrackHeight::Small: return 0.55;
        case TrackHeight::Medium: return 1.0;
        case TrackHeight::Large: return 1.7;
        case TrackHeight::Jumbo: return 2.8;
    }
    return 1.0;
}

class RowPolicy {
public:
    virtual ~RowPolicy() = default;
    // False while every row is plain, so the controller keeps its uniform
    // geometry (no per-row vectors) for a session nobody has resized.
    virtual bool active() const = 0;
    virtual RowSpec row(int index, std::uint64_t track) const = 0;
};

}  // namespace sonora::ui
