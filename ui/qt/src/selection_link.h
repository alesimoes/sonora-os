// SPDX-License-Identifier: GPL-3.0-or-later
//
// Link Timeline and Edit Selection (Pro Tools' toggle, Shift+/): when
// linked, the edit selection's range and the timeline's (loop/playback)
// range are one range; unlinked, each moves alone. Also the tracks the
// operator added to the selection with Ctrl-click, apart from the
// contiguous block the Selection itself covers.
//
// Interface state, never the session's: it lives in a file of its own beside
// the window layouts. Plain C++, no display and no Qt, because EditController
// (a Qt-free target) holds it (ADR-0167).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>

namespace sonora::ui {

struct TimelineRange {
    std::int64_t start = 0, end = 0;
    bool empty() const { return end <= start; }
};

// $SONORA_SELECTION_LINK_FILE, else $XDG_CONFIG_HOME/sonora/selection-link.json.
inline std::string default_selection_link_file() {
    if (const char* f = std::getenv("SONORA_SELECTION_LINK_FILE"); f && *f) return f;
    std::string base;
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) {
        base = x;
    } else if (const char* h = std::getenv("HOME"); h && *h) {
        base = std::string(h) + "/.config";
    }
    return base + "/sonora/selection-link.json";
}

class SelectionLink {
public:
    // Pro Tools ships linked.
    bool linked() const { return linked_; }
    void setLinked(bool on) { linked_ = on; }
    bool toggle() { return linked_ = !linked_; }

    // --- the timeline's own range ---
    const TimelineRange& timeline() const { return timeline_; }
    // The edit selection [start, end) as the timeline range it implies: only
    // when linked and the selection is a range, else nothing to mirror.
    std::optional<TimelineRange> range_for_timeline(std::int64_t start, std::int64_t end) {
        if (!linked_ || end <= start) return std::nullopt;
        timeline_ = {start, end};
        return timeline_;
    }
    // The timeline's range was set (loop brackets dragged). Always kept; true
    // when the edit selection must follow it, which is when linked.
    bool apply_timeline_range(std::int64_t start, std::int64_t end) {
        timeline_ = {std::min(start, end), std::max(start, end)};
        return linked_;
    }

    // --- Ctrl-click, non-contiguous tracks (display indices) ---
    const std::set<int>& extra_tracks() const { return extra_; }
    // Adds the track, or takes it off when it was already there; true if in.
    bool toggle_track(int index) {
        if (index < 0) return false;
        if (extra_.erase(index) == 0) extra_.insert(index);
        return extra_.count(index) != 0;
    }
    void clear_tracks() { extra_.clear(); }
    // After tracks were removed: indices at or past `track_count` are gone.
    void prune(int track_count) {
        extra_.erase(extra_.lower_bound(track_count), extra_.end());
    }

    // --- persistence: only `linked`; a range or a track set belongs to the
    // session being edited and is not kept across runs ---
    std::string toJson() const { return std::string("{\"linked\":") + (linked_ ? "true" : "false") + "}"; }
    // A file that does not say true or false leaves the state as it was.
    bool fromJson(const std::string& text, std::string& why) {
        const auto key = text.find("\"linked\"");
        const auto colon = key == std::string::npos ? key : text.find(':', key);
        if (colon == std::string::npos) {
            why = "selection link file has no \"linked\" flag";
            return false;
        }
        const auto v = text.find_first_not_of(" \t\r\n", colon + 1);
        if (v != std::string::npos && text.compare(v, 4, "true") == 0) {
            linked_ = true;
        } else if (v != std::string::npos && text.compare(v, 5, "false") == 0) {
            linked_ = false;
        } else {
            why = "selection link file: \"linked\" is not true or false";
            return false;
        }
        return true;
    }
    // A missing file is not a fault on load.
    bool load(const std::string& path, std::string& why) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return true;
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            why = "cannot read " + path;
            return false;
        }
        std::ostringstream all;
        all << f.rdbuf();
        return fromJson(all.str(), why);
    }
    bool save(const std::string& path, std::string& why) const {
        std::error_code ec;
        const auto dir = std::filesystem::path(path).parent_path();
        if (!dir.empty()) std::filesystem::create_directories(dir, ec);
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f || !(f << toJson())) {
            why = "cannot write " + path;
            return false;
        }
        return true;
    }

private:
    bool linked_ = true;
    TimelineRange timeline_;
    std::set<int> extra_;
};

}  // namespace sonora::ui
