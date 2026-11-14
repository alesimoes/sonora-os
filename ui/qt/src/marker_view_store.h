// SPDX-License-Identifier: GPL-3.0-or-later
//
// The seam EditController keeps a marker's stored view behind (ADR-0137):
// plain C++, so the Edit controller's library needs no Qt. MarkerViews
// (marker_views.h) is the store the interface uses, kept in a file.
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace sonora::ui {

struct MarkerView {
    double fpp = 0.0;          // frames per pixel
    double start = 0.0;        // frame at the left edge
    double track_height = 0.0; // px
    std::vector<std::uint64_t> hidden;  // track ids
    bool recall_zoom = true;
    bool recall_tracks = true;
};

class MarkerViewStore {
public:
    virtual ~MarkerViewStore() = default;
    virtual const MarkerView* find(const std::string& session, std::uint64_t marker) const = 0;
    // Replaces the captured view; the two toggles keep what the operator set.
    virtual void store(const std::string& session, std::uint64_t marker, MarkerView v) = 0;
    virtual bool clear(const std::string& session, std::uint64_t marker) = 0;
    // False when the marker has no stored view: a toggle on nothing lies.
    virtual bool setRecallZoom(const std::string& session, std::uint64_t marker, bool on) = 0;
    virtual bool setRecallTracks(const std::string& session, std::uint64_t marker, bool on) = 0;
    // Drops the entries of `session` whose marker is not in `live`; how many went.
    virtual int prune(const std::string& session, const std::set<std::uint64_t>& live) = 0;
};

}  // namespace sonora::ui
