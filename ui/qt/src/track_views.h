// SPDX-License-Identifier: GPL-3.0-or-later
//
// Track view (ADR-0106): the height preset of each track's row and whether
// it is hidden, kept per session id. Interface state, never the session's:
// how the operator looks at a session is not in the session. Per-session
// state resets when a session opens: a store hands out a view only for the
// id it was asked about, so one session's hidden tracks never leak into the
// next.
//
// Qt-free and display-free; the tests drive it directly.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace sonora::ui {

enum class TrackHeight { Small, Medium, Large, Jumbo };

inline const char* track_height_name(TrackHeight h) {
    switch (h) {
        case TrackHeight::Small: return "small";
        case TrackHeight::Medium: return "medium";
        case TrackHeight::Large: return "large";
        case TrackHeight::Jumbo: return "jumbo";
    }
    return "medium";
}

// False for anything but the four names: an unknown preset is refused, not
// guessed.
inline bool parse_track_height(const std::string& name, TrackHeight& out) {
    for (TrackHeight h : {TrackHeight::Small, TrackHeight::Medium, TrackHeight::Large,
                          TrackHeight::Jumbo}) {
        if (name == track_height_name(h)) {
            out = h;
            return true;
        }
    }
    return false;
}

struct TrackView {
    TrackHeight height = TrackHeight::Medium;
    bool hidden = false;
};

// Keyed by session id, then track id.
class TrackViewStore {
public:
    virtual ~TrackViewStore() = default;
    virtual TrackView get(const std::string& session_id, uint32_t track) const = 0;
    virtual void set(const std::string& session_id, uint32_t track, const TrackView& v) = 0;
    // Shows every hidden track; heights stay. Returns how many were shown.
    virtual uint32_t show_all(const std::string& session_id) = 0;
    virtual void forget(const std::string& session_id) = 0;
};

class FakeTrackViewStore final : public TrackViewStore {
public:
    TrackView get(const std::string& session_id, uint32_t track) const override {
        auto s = views_.find(session_id);
        if (s == views_.end()) return {};
        auto t = s->second.find(track);
        return t == s->second.end() ? TrackView{} : t->second;
    }
    void set(const std::string& session_id, uint32_t track, const TrackView& v) override {
        views_[session_id][track] = v;
    }
    uint32_t show_all(const std::string& session_id) override {
        uint32_t n = 0;
        auto s = views_.find(session_id);
        if (s == views_.end()) return 0;
        for (auto& [id, v] : s->second) {
            if (v.hidden) {
                v.hidden = false;
                ++n;
            }
        }
        return n;
    }
    void forget(const std::string& session_id) override { views_.erase(session_id); }

private:
    std::map<std::string, std::map<uint32_t, TrackView>> views_;
};

// The store a machine keeps: one JSON file beside the preferences, every
// session's views in it by session id (ADR-0106). Per machine, never in the
// session file: opening the session elsewhere shows it plain.
//
// An empty path keeps it in memory (a picture, a test).
class TrackViews final : public TrackViewStore {
public:
    // $SONORA_TRACK_VIEWS_FILE, else $XDG_CONFIG_HOME/sonora/track-views.json.
    static std::string default_path();

    explicit TrackViews(std::string path = {}) : path_(std::move(path)) {}

    // A missing file is not a fault. A file that does not parse, or has an
    // unexpected shape, is ignored whole and `why` says so: a view is never
    // worth refusing to start for. Entries naming an unknown height are
    // dropped one by one.
    bool load(std::string& why);

    TrackView get(const std::string& session_id, uint32_t track) const override;
    void set(const std::string& session_id, uint32_t track, const TrackView& v) override;
    uint32_t show_all(const std::string& session_id) override;
    void forget(const std::string& session_id) override;

    // Why the last write failed, empty when it succeeded. The change itself
    // stands for this run; the model holds the text for the operator.
    const std::string& save_error() const { return save_error_; }
    // The tracks with a non-default view in `session_id`.
    uint32_t customised(const std::string& session_id) const;

private:
    void save();

    std::string path_;
    std::string save_error_;
    std::map<std::string, std::map<uint32_t, TrackView>> views_;
};

}  // namespace sonora::ui
