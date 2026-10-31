// SPDX-License-Identifier: GPL-3.0-or-later
#include "edit_clipboard.h"

#include <algorithm>
#include <limits>
#include <map>

namespace sonora::ui {

using namespace model;

bool Clipboard::empty() const {
    return std::all_of(lanes.begin(), lanes.end(), [](const Clip& c) { return c.empty(); });
}

bool copy_range(const Snapshot& s, const TrackEdit& track, Frames a, Frames b, Clip& out) {
    const Playlist* p = active_playlist(&track);
    if (p == nullptr || b <= a) return false;
    Clip clip;
    clip.length = b - a;
    clip.channels = track.channels;
    clip.track = track.name;
    for (const Region& r : p->regions) {
        if (r.position >= b || r.end() <= a) continue;
        const Frames from = std::max(a, r.position), to = std::min(b, r.end());
        Region c = r;
        c.source_offset += from - r.position;
        c.position = from - a;
        c.length = to - from;
        // A fade whose end of the region the range cut away is not copied:
        // half a fade is a level change nobody drew.
        if (from > r.position) c.fade_in.length = 0;
        if (to < r.end()) c.fade_out.length = 0;
        c.fade_in.length = std::min(c.fade_in.length, c.length);
        c.fade_out.length = std::min(c.fade_out.length, c.length - c.fade_in.length);
        clip.regions.push_back(std::move(c));
        for (const SourceRef& ch : r.channels) {
            const bool known = std::any_of(clip.sources.begin(), clip.sources.end(),
                                           [&ch](const Source& x) { return x.id == ch.source; });
            if (known) continue;
            if (const Source* src = s.sources->find(ch.source)) clip.sources.push_back(*src);
        }
    }
    if (clip.regions.empty()) {
        out = std::move(clip);
        return false;
    }
    const auto in_clip = [&clip](Id id) {
        return std::any_of(clip.regions.begin(), clip.regions.end(),
                           [id](const Region& r) { return r.id == id; });
    };
    for (const Crossfade& x : p->crossfades) {
        if (!in_clip(x.left) || !in_clip(x.right)) continue;
        const auto left = std::find_if(p->regions.begin(), p->regions.end(),
                                       [&x](const Region& r) { return r.id == x.left; });
        if (left == p->regions.end()) continue;
        Frames before = 0, after = 0;
        crossfade_extent(x, before, after);
        // Only a crossfade the range holds whole: its pieces then keep the
        // handles it draws on, so the paste validates as the original did.
        if (left->end() - before >= a && left->end() + after <= b) clip.crossfades.push_back(x);
    }
    out = std::move(clip);
    return true;
}

void clear_range(const Playlist& p, Frames a, Frames b, const NextId& id,
                 std::vector<Member>& out) {
    if (b <= a) return;
    for (const Region& r : p.regions) {
        if (r.position >= b || r.end() <= a) continue;
        const bool head = r.position < a, tail = r.end() > b;
        // A trim only clamps fades, so a cut edge is cleared of its fade
        // here, as overwrite does: kept, it would fade out audio that
        // played at full level before the edit.
        if (head && tail) {
            // The right part keeps what lies past the range; the left one
            // is trimmed back to where the range starts. The split already
            // took the fades off both new edges.
            out.push_back(SplitRegion{p.id, r.id, b, id()});
            out.push_back(TrimRegion{p.id, r.id, Edge::End, a});
        } else if (head) {
            // Before the trim, which clamps the kept fade against this one.
            if (r.fade_out.length > 0) out.push_back(SetFade{p.id, r.id, FadeEnd::Out, Fade{}});
            out.push_back(TrimRegion{p.id, r.id, Edge::End, a});
        } else if (tail) {
            if (r.fade_in.length > 0) out.push_back(SetFade{p.id, r.id, FadeEnd::In, Fade{}});
            out.push_back(TrimRegion{p.id, r.id, Edge::Start, b});
        } else {
            out.push_back(RemoveRegion{p.id, r.id});
        }
    }
}

void paste_clip(const Playlist& p, Frames at, const Clip& clip, const NextId& id,
                std::vector<Member>& out) {
    repeat_clip(p, at, clip, 1, id, out);
}

void repeat_clip(const Playlist& p, Frames at, const Clip& clip, int count, const NextId& id,
                 std::vector<Member>& out) {
    if (count < 1) return;
    clear_range(p, at, at + clip.length * count, id, out);
    for (int i = 0; i < count; ++i) {
        const Frames origin = at + clip.length * i;
        std::map<Id, Id> renamed;
        for (const Region& r : clip.regions) {
            Region placed = r;
            placed.id = id();
            placed.position = origin + r.position;
            renamed[r.id] = placed.id;
            // Nothing is beneath any more, so no split id is needed.
            out.push_back(PlaceRegion{p.id, std::move(placed), kNoId});
        }
        for (Crossfade x : clip.crossfades) {
            x.left = renamed[x.left];
            x.right = renamed[x.right];
            out.push_back(SetCrossfade{p.id, x});
        }
    }
}

std::size_t repeat_members(const Playlist& p, Frames at, const Clip& clip, std::size_t count) {
    if (count == 0 || clip.length <= 0) return 0;
    // Nothing lies past the playlist's last region, so the span cleared is
    // capped there rather than multiplied out for a count that could
    // overflow it.
    Frames last = at;
    for (const Region& r : p.regions) last = std::max(last, r.end());
    const auto reach = static_cast<std::size_t>((last - at + clip.length - 1) / clip.length);
    const auto copies = static_cast<Frames>(std::min(count, reach));
    std::vector<Member> clear;
    clear_range(p, at, at + clip.length * copies, [] { return kNoId; }, clear);
    return clear.size() + count * (clip.regions.size() + clip.crossfades.size());
}

namespace {

std::string channels_text(std::uint16_t n) {
    return n == 1 ? "mono" : n == 2 ? "stereo" : std::to_string(n) + "-channel";
}

}  // namespace

std::string paste_problem(const Snapshot& s, const TrackEdit& track, const Clip& clip) {
    if (clip.empty()) return "the clipboard is empty; cut or copy a selection first";
    if (clip.channels != track.channels) {
        const std::string from = clip.track.empty() ? "" : " from " + clip.track;
        return "the clipboard holds " + channels_text(clip.channels) + " audio" + from + " and " +
               track.name + " is " + channels_text(track.channels) + "; nothing was pasted";
    }
    for (const Source& src : clip.sources) {
        const Source* here = s.sources->find(src.id);
        if (here == nullptr || !(*here == src)) {
            return "the clipboard's audio was copied from another session and is not in this "
                   "one; nothing was pasted";
        }
    }
    return {};
}

bool parse_repeat_count(const std::string& text, int& out) {
    std::size_t i = 0, n = text.size();
    while (i < n && text[i] == ' ') ++i;
    while (n > i && text[n - 1] == ' ') --n;
    bool negative = false;
    if (i < n && (text[i] == '-' || text[i] == '+')) negative = text[i++] == '-';
    if (i == n) return false;
    long long v = 0;
    for (; i < n; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        v = v * 10 + (text[i] - '0');
        if (v > std::numeric_limits<int>::max()) return false;
    }
    out = static_cast<int>(negative ? -v : v);
    return true;
}

std::string over_limit(const char* gesture, const char* nothing_was, std::size_t members) {
    if (members <= kMaxCompoundMembers) return {};
    return std::string(gesture) + " needs " + std::to_string(members) + " changes; at most " +
           std::to_string(kMaxCompoundMembers) + " in one edit. Nothing was " + nothing_was + ".";
}

std::string step_text(const std::string& label) {
    static const std::map<std::string, const char*> kWords = {
        {"AddTrack", "New Track"},        {"PlaceRegion", "Place Region"},
        {"RecordTake", "Record"},         {"RecordLoopTake", "Loop Record"},
        {"MoveRegion", "Move"},
        {"TrimRegion", "Trim"},           {"SplitRegion", "Separate"},
        {"RemoveRegion", "Clear"},        {"SetFade", "Fade"},
        {"SetCrossfade", "Crossfade"},    {"RemoveCrossfade", "Clear Crossfade"},
        {"AddPlaylist", "New Playlist"},  {"SetActivePlaylist", "Choose Playlist"},
        {"AddMarker", "New Marker"},      {"MoveMarker", "Move Marker"},
        {"RemoveMarker", "Clear Marker"}, {"SetTrackName", "Rename Track"},
        {"ReorderTracks", "Reorder Tracks"}, {"AddInsert", "Insert Plug-in"},
        {"RemoveInsert", "Remove Plug-in"},  {"MoveInsert", "Move Plug-in"},
        {"SetInsertActive", "Plug-in Active State"}, {"ReplaceInsert", "Replace Plug-in"},
        {"RemoveTrack", "Delete Track"},  {"DuplicateTrack", "Duplicate Track"},
        {"SetOutput", "Set Output"},
    };
    const auto it = kWords.find(label);
    return it != kWords.end() ? it->second : label;
}

}  // namespace sonora::ui
