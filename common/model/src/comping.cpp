// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/model/comping.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <string>
#include <utility>

namespace sonora::model {

namespace {

const TrackEdit* find_track(const Snapshot& s, Id id) {
    for (const auto& t : s.edit->tracks) {
        if (t->id == id) return t.get();
    }
    return nullptr;
}

const Playlist* find_playlist(const TrackEdit& t, Id id) {
    for (const auto& p : t.playlists) {
        if (p->id == id) return p.get();
    }
    return nullptr;
}

// Leaves [a, b) of `p` empty with the fewest members: one trim for a region
// cut at one edge, a split and a trim for one cut at both, one removal for
// one inside. The same rule as the interface's Clear (edit_clipboard.cpp).
void clear(const Playlist& p, Frames a, Frames b, const std::function<Id()>& next_id,
           std::vector<Member>& out) {
    for (const Region& r : p.regions) {
        if (r.position >= b || r.end() <= a) continue;
        const bool head = r.position < a, tail = r.end() > b;
        if (head && tail) {
            // The split takes the fades off both new edges.
            out.push_back(SplitRegion{p.id, r.id, b, next_id()});
            out.push_back(TrimRegion{p.id, r.id, Edge::End, a});
        } else if (head) {
            // A trim only clamps fades: kept, the fade-out would now fade
            // audio that played at full level before the swipe.
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

// `r`'s part of [a, b) at its own place, as region `id`. Half a fade is a
// level change nobody drew, so a fade on an edge the range cut goes.
Region piece(const Region& r, Frames a, Frames b, Id id) {
    const Frames from = std::max(a, r.position), to = std::min(b, r.end());
    Region c = r;
    c.id = id;
    c.source_offset += from - r.position;
    c.position = from;
    c.length = to - from;
    if (from > r.position) c.fade_in = Fade{};
    if (to < r.end()) c.fade_out = Fade{};
    c.fade_in.length = std::min(c.fade_in.length, c.length);
    c.fade_out.length = std::min(c.fade_out.length, c.length - c.fade_in.length);
    return c;
}

const Region* region_at(const Playlist& p, Id id) {
    for (const Region& r : p.regions) {
        if (r.id == id) return &r;
    }
    return nullptr;
}

}  // namespace

bool comp_range(const Snapshot& s, const CompRange& c, const std::function<Id()>& next_id,
                Comp& out, std::string& why) {
    out = Comp{};
    why.clear();
    const TrackEdit* t = find_track(s, c.track);
    if (t == nullptr) {
        why = "no such track; nothing was copied";
        return false;
    }
    if (t->kind != TrackKind::Audio) {
        why = t->name + " is a MIDI track, and MIDI takes cannot be comped yet; nothing was copied";
        return false;
    }
    const Playlist* from = find_playlist(*t, c.from);
    const Playlist* to = find_playlist(*t, c.to);
    if (from == nullptr || to == nullptr) {
        why = "the take or the comp is not a playlist of " + t->name + "; nothing was copied";
        return false;
    }
    if (from == to) {
        why = "the take and the comp are the same playlist; nothing was copied";
        return false;
    }
    if (c.start < 0 || c.end <= c.start) {
        why = "the range to copy is empty; nothing was copied";
        return false;
    }

    std::vector<Member> m;
    clear(*to, c.start, c.end, next_id, m);
    std::map<Id, Id> renamed;
    for (const Region& r : from->regions) {
        if (r.position >= c.end || r.end() <= c.start) continue;
        const Id id = next_id();
        renamed[r.id] = id;
        // Nothing is beneath any more, so no split id is needed.
        m.push_back(PlaceRegion{to->id, piece(r, c.start, c.end, id), 0});
    }
    for (Crossfade x : from->crossfades) {
        const auto l = renamed.find(x.left), r = renamed.find(x.right);
        if (l == renamed.end() || r == renamed.end()) continue;
        const Region* left = region_at(*from, x.left);
        if (left == nullptr) continue;
        Frames before = 0, after = 0;
        crossfade_extent(x, before, after);
        // Only a crossfade the range holds whole: cut, its pieces would
        // lack the handles it draws on.
        if (left->end() - before < c.start || left->end() + after > c.end) continue;
        x.left = l->second;
        x.right = r->second;
        m.push_back(SetCrossfade{to->id, x});
    }
    if (m.size() > kMaxCompoundMembers) {
        why = "Copy to Playlist needs " + std::to_string(m.size()) + " changes; at most " +
              std::to_string(kMaxCompoundMembers) + " in one edit. Nothing was copied.";
        return false;
    }
    if (m.empty()) return true;

    // The joins are decided on the edit as built, so the model's own rules
    // say which crossfade fits rather than a second copy of them here.
    Snapshot built;
    std::string error;
    if (!apply(s, Compound{m, {}}, built, error)) {
        why = "the copy was refused: " + error;
        return false;
    }
    const auto placed = [&renamed](Id id) {
        return std::any_of(renamed.begin(), renamed.end(),
                           [id](const std::pair<const Id, Id>& e) { return e.second == id; });
    };
    std::vector<std::pair<Id, Id>> seams;   // left, right
    const Playlist* comp = find_playlist(*find_track(built, c.track), c.to);
    for (std::size_t i = 0; i + 1 < comp->regions.size(); ++i) {
        const Region& l = comp->regions[i];
        const Region& r = comp->regions[i + 1];
        if (l.end() != r.position) continue;
        const bool at_start = r.position == c.start && placed(r.id) && !placed(l.id);
        const bool at_end = l.end() == c.end && placed(l.id) && !placed(r.id);
        if (at_start || at_end) seams.emplace_back(l.id, r.id);
    }
    out.seams = seams.size();
    if (c.seam_crossfade > 0) {
        for (const auto& [left_id, right_id] : seams) {
            const Playlist* p = find_playlist(*find_track(built, c.track), c.to);
            const Region* l = region_at(*p, left_id);
            const Region* r = region_at(*p, right_id);
            Frames before = 0, after = 0;
            crossfade_handles(*l, *r, *built.sources, before, after);
            // Half of each region at most, so the two joins of a short
            // piece never meet inside it.
            before = std::min(before, l->length / 2);
            after = std::min(after, r->length / 2);
            Crossfade x;
            x.left = left_id;
            x.right = right_id;
            x.length = c.seam_crossfade;
            x.align = Align::Centre;
            x.shape = FadeShape::EqualPower;
            if (m.size() >= kMaxCompoundMembers || !fit_crossfade(x, before, after)) continue;
            Snapshot faded;
            const SetCrossfade set{c.to, x};
            if (!apply(built, set, faded, error)) continue;
            built = std::move(faded);
            m.push_back(set);
            ++out.seams_faded;
        }
    }
    out.members = std::move(m);
    return true;
}

bool consolidate_range(const Snapshot& s, const ConsolidateRange& c,
                       const std::function<Id()>& next_id, Comp& out, std::string& why) {
    out = Comp{};
    why.clear();
    const TrackEdit* t = find_track(s, c.track);
    if (t == nullptr) {
        why = "no such track; nothing was consolidated";
        return false;
    }
    if (t->kind != TrackKind::Audio) {
        why = t->name + " is a MIDI track, and only audio is consolidated; nothing was consolidated";
        return false;
    }
    const Playlist* p = find_playlist(*t, c.playlist);
    if (p == nullptr) {
        why = "the range is not a playlist of " + t->name + "; nothing was consolidated";
        return false;
    }
    if (c.start < 0 || c.end <= c.start) {
        why = "the range to consolidate is empty; nothing was consolidated";
        return false;
    }
    const bool any = std::any_of(p->regions.begin(), p->regions.end(), [&c](const Region& r) {
        return r.position < c.end && r.end() > c.start;
    });
    if (!any) {
        why = "nothing to consolidate: the range holds no region";
        return false;
    }
    const auto src = std::find_if(s.sources->sources.begin(), s.sources->sources.end(),
                                  [&c](const Source& e) { return e.id == c.source; });
    if (src == s.sources->sources.end()) {
        why = "the rendered file is not in the session; nothing was consolidated";
        return false;
    }
    if (src->channels != t->channels) {
        why = "the rendered file has " + std::to_string(src->channels) + " channels, " + t->name +
              " has " + std::to_string(t->channels) + "; nothing was consolidated";
        return false;
    }
    if (src->length < c.end - c.start) {
        why = "the rendered file is shorter than the range; nothing was consolidated";
        return false;
    }
    Region r;
    r.id = next_id();
    r.name = c.name;
    r.position = c.start;
    r.length = c.end - c.start;
    r.source_offset = 0;
    for (std::uint16_t ch = 0; ch < t->channels; ++ch) r.channels.push_back({c.source, ch});
    // The overwrite may split a region that spans the range; its id is
    // reserved whether or not it is used.
    out.members.push_back(PlaceRegion{c.playlist, std::move(r), next_id()});
    return true;
}

Id comp_origin(const TrackEdit& track, const Region& r, Id exclude) {
    for (const auto& p : track.playlists) {
        if (p->id == exclude) continue;
        // Regions never overlap, so only the last one starting at or before
        // r can cover it.
        const auto after = std::upper_bound(
            p->regions.begin(), p->regions.end(), r.position,
            [](Frames at, const Region& q) { return at < q.position; });
        if (after == p->regions.begin()) continue;
        const Region& q = *std::prev(after);
        if (q.end() >= r.end() && q.channels == r.channels &&
            q.source_offset - q.position == r.source_offset - r.position) {
            return p->id;
        }
    }
    return 0;
}

}  // namespace sonora::model
