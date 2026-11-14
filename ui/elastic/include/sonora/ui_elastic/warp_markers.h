// SPDX-License-Identifier: GPL-3.0-or-later
//
// The warp markers the operator places on one region before Apply (ADR-0136
// §1): held by the editor only, never in the session, dropped when the
// region changes. Qt-free, so the Warp panel is given one and the tests
// drive it without a window. Every edit keeps the contract's rule (both
// sides strictly increasing, the region's start the implicit 0:0), so what
// the panel lists is always a request the engine accepts.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sonora/api/elastic.h"

namespace sonora::ui {

// One line of the panel's marker list, as the operator reads it.
struct WarpMarkerRow {
    std::string from;   // where the marker sits in the region now, "1.250 s"
    std::string to;     // where it lands, "1.400 s"
    std::string speed;  // the stretch of the stretch leading into it, "x1.12"
};

class WarpMarkers {
public:
    // No region: every edit is refused until reset() names one.
    WarpMarkers() = default;
    // A new region (its id and length in frames): the markers of the last one
    // are dropped, since they were frames of a different take.
    void reset(std::uint64_t region, std::int64_t length);
    std::uint64_t region() const { return region_; }
    std::int64_t length() const { return length_; }

    const std::vector<api::WarpMarker>& markers() const { return markers_; }
    bool empty() const { return markers_.empty(); }

    // A marker at `source` landing at `target`, both frames from the
    // region's start. Refused, with the reason, past the region, at or
    // before its start, on a source that already has one, or where it would
    // cross a neighbour's target (time would run backwards).
    bool add(std::int64_t source, std::int64_t target, std::string& why);
    // Marker `index` lands at `target` instead; the same refusals.
    bool move(std::size_t index, std::int64_t target, std::string& why);
    bool remove(std::size_t index, std::string& why);
    void clear() { markers_.clear(); }

    // The stretched region's length in frames.
    std::int64_t output_length() const;
    // How much the stretch ending at marker `index` is slowed: its new
    // length over its old one (2 is twice as long).
    double speed(std::size_t index) const;

    // The tempo.edit words for these markers ("elastic warp 1:2,3:4 rhythmic");
    // refused with no marker, since a warp that moves nothing renders a copy
    // for nothing.
    bool words(api::ElasticAlgorithm algorithm, std::string& out, std::string& why) const;
    // The panel's lines, times at `sample_rate`.
    std::vector<WarpMarkerRow> rows(std::uint32_t sample_rate) const;

private:
    // `prev` and `next` are the neighbours the marker sits between; null
    // for the region's start (the implicit 0:0) and for none after it.
    bool source_fits(const api::WarpMarker* prev, const api::WarpMarker* next,
                     std::int64_t source, std::string& why) const;
    bool target_fits(const api::WarpMarker* prev, const api::WarpMarker* next, api::WarpMarker m,
                     std::string& why) const;

    std::uint64_t region_ = 0;
    std::int64_t length_ = 0;
    std::vector<api::WarpMarker> markers_;
};

}  // namespace sonora::ui
