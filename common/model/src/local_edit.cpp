// SPDX-License-Identifier: GPL-3.0-or-later
#include "local_edit.h"

#include <algorithm>
#include <cstdint>
#include <iterator>

#include "checks.h"

namespace sonora::model::detail {

namespace {

// A neighbour of a touched region is at most this far from its range: the
// region beside it, and the far side of the crossfade on that one.
constexpr std::size_t kNear = 3;

using Ids = std::vector<Id>;

void sort_unique(Ids& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

bool has(const Ids& sorted, Id id) { return std::binary_search(sorted.begin(), sorted.end(), id); }

std::size_t slot(const Ids& sorted, Id id) {
    return static_cast<std::size_t>(
        std::lower_bound(sorted.begin(), sorted.end(), id) - sorted.begin());
}

const Region* at(const Playlist& p, std::size_t i) {
    return i == kNoIndex ? nullptr : &p.regions[i];
}

// The touched regions and one either side, then the recorded ends: every
// region whose own checks, adjacency or crossfades an edit could change.
std::vector<std::size_t> near_indices(const Playlist& p, const Touched& t) {
    const std::size_t n = p.regions.size();
    std::vector<std::size_t> out;
    for (const Touched::Range& r : t.ranges()) {
        const std::size_t lo = r.first == 0 ? 0 : r.first - 1;
        const std::size_t hi = std::min(r.second + 1, n);
        for (std::size_t i = lo; i < hi; ++i) out.push_back(i);
    }
    for (const Id id : t.ends()) {
        const std::size_t i = find_index(p, id, t);
        if (i != kNoIndex) out.push_back(i);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

Ids ids_at(const Playlist& p, const std::vector<std::size_t>& indices) {
    Ids ids;
    ids.reserve(indices.size());
    for (const std::size_t i : indices) ids.push_back(p.regions[i].id);
    sort_unique(ids);
    return ids;
}

}  // namespace

void clamp_fades(Region& r) {
    r.fade_out.length = std::min(r.fade_out.length, r.length);
    r.fade_in.length = std::min(r.fade_in.length, r.length - r.fade_out.length);
}

bool audited() {
#if SONORA_MODEL_AUDIT
    return true;
#else
    return false;
#endif
}

void Touched::clear() {
    ranges_.clear();
    gone_.clear();
    ends_.clear();
}

void Touched::splice(std::size_t at, std::size_t removed, std::size_t inserted) {
    const auto shift = [&](std::size_t i) {
        return static_cast<std::size_t>(static_cast<std::ptrdiff_t>(i) +
                                        static_cast<std::ptrdiff_t>(inserted) -
                                        static_cast<std::ptrdiff_t>(removed));
    };
    // Merged in the coordinates from before the splice; a range that touches
    // [at, at + removed] joins it, so ranges stay disjoint and sorted.
    std::size_t lo = at;
    std::size_t hi = at + removed;
    std::vector<Range> kept;
    kept.reserve(ranges_.size() + 1);
    for (const Range& r : ranges_) {
        if (r.second < at) {
            kept.push_back(r);
        } else if (r.first > at + removed) {
            kept.push_back({shift(r.first), shift(r.second)});
        } else {
            lo = std::min(lo, r.first);
            hi = std::max(hi, r.second);
        }
    }
    kept.push_back({lo, shift(hi)});
    std::sort(kept.begin(), kept.end());
    ranges_ = std::move(kept);
}

std::size_t find_index(const Playlist& p, Id id, const Touched& t) {
    const std::size_t n = p.regions.size();
    for (const Touched::Range& r : t.ranges()) {
        const std::size_t lo = r.first > kNear ? r.first - kNear : 0;
        const std::size_t hi = std::min(r.second + kNear, n);
        for (std::size_t i = lo; i < hi; ++i) {
            if (p.regions[i].id == id) return i;
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (p.regions[i].id == id) return i;
    }
    return kNoIndex;
}

bool overwrite_range(Playlist& p, Frames a, Frames b, Id split_id, bool& used_split,
                     std::string& error, Touched& t) {
    auto& rs = p.regions;
    // Regions do not overlap, so their ends are sorted as their starts are.
    const auto first = std::partition_point(rs.begin(), rs.end(),
                                            [a](const Region& r) { return r.end() <= a; });
    auto last = first;
    while (last != rs.end() && last->position < b) ++last;
    if (first == last) return true;

    std::vector<Region> kept;
    for (auto it = first; it != last; ++it) {
        Region& r = *it;
        const Frames s = r.position;
        const Frames e = r.end();
        if (s >= a && e <= b) {
            // Covered: removed. Its source stays in the pool.
            t.add_gone(r.id);
        } else if (s < a && e > b) {
            if (split_id == 0) {
                error = "region " + std::to_string(r.id) +
                        " spans the whole range and must be split, but the"
                        " operation carries no split id";
                return false;
            }
            Region right = r;
            right.id = split_id;
            right.position = b;
            right.source_offset += b - s;
            right.length = e - b;
            right.fade_in = Fade{};
            clamp_fades(right);
            // The crossfade at r's old end now belongs to the right part.
            for (Crossfade& x : p.crossfades) {
                if (x.left == r.id) x.left = split_id;
            }
            r.length = a - s;
            r.fade_out = Fade{};
            clamp_fades(r);
            kept.push_back(std::move(r));
            kept.push_back(std::move(right));
            used_split = true;
        } else if (s < a) {
            r.length = a - s;
            r.fade_out = Fade{};
            clamp_fades(r);
            kept.push_back(std::move(r));
        } else {
            r.source_offset += b - s;
            r.length = e - b;
            r.position = b;
            r.fade_in = Fade{};
            clamp_fades(r);
            kept.push_back(std::move(r));
        }
    }
    // Replaced where they were: one shift of the regions after them at most.
    const auto at = static_cast<std::size_t>(first - rs.begin());
    const auto removed = static_cast<std::size_t>(last - first);
    const std::size_t common = std::min(removed, kept.size());
    std::move(kept.begin(), kept.begin() + static_cast<std::ptrdiff_t>(common), first);
    const auto tail = rs.begin() + static_cast<std::ptrdiff_t>(at + common);
    if (kept.size() < removed) {
        rs.erase(tail, rs.begin() + static_cast<std::ptrdiff_t>(at + removed));
    } else {
        rs.insert(tail, std::make_move_iterator(kept.begin() + static_cast<std::ptrdiff_t>(common)),
                  std::make_move_iterator(kept.end()));
    }
    t.splice(at, removed, kept.size());
    return true;
}

void insert_region(Playlist& p, Region r, Touched& t) {
    const auto it = std::lower_bound(
        p.regions.begin(), p.regions.end(), r.position,
        [](const Region& a, Frames pos) { return a.position < pos; });
    const auto i = static_cast<std::size_t>(it - p.regions.begin());
    p.regions.insert(it, std::move(r));
    t.splice(i, 0, 1);
}

void erase_region(Playlist& p, std::size_t i, Touched& t) {
    p.regions.erase(p.regions.begin() + static_cast<std::ptrdiff_t>(i));
    t.splice(i, 1, 0);
}

void drop_broken_crossfades_near(Playlist& p, const SourcePool& pool, Touched& t) {
    if (p.crossfades.empty()) return;
    const std::size_t n = p.regions.size();
    Ids gone = t.gone();
    sort_unique(gone);
    Ids changed = gone;
    for (const Touched::Range& r : t.ranges()) {
        for (std::size_t i = r.first; i < std::min(r.second, n); ++i) {
            changed.push_back(p.regions[i].id);
        }
    }
    sort_unique(changed);
    const auto decided = [&](const Crossfade& x) {
        return has(changed, x.left) || has(changed, x.right);
    };
    // Every region a decided crossfade names: the whole-playlist rule reads
    // what the crossfades before it left on those regions.
    Ids named;
    for (const Crossfade& x : p.crossfades) {
        if (decided(x)) {
            named.push_back(x.left);
            named.push_back(x.right);
        }
    }
    if (named.empty()) return;
    sort_unique(named);
    std::vector<std::pair<Frames, Frames>> used(named.size());   // in_after, out_before

    std::size_t out = 0;
    for (std::size_t k = 0; k < p.crossfades.size(); ++k) {
        const Crossfade x = p.crossfades[k];
        const bool named_left = has(named, x.left);
        const bool named_right = has(named, x.right);
        Frames before = 0, after = 0;
        crossfade_extent(x, before, after);
        bool keep = true;
        if (decided(x)) {
            const Region* l = at(p, find_index(p, x.left, t));
            const Region* r = at(p, find_index(p, x.right, t));
            const auto& lu = used[slot(named, x.left)];
            const auto& ru = used[slot(named, x.right)];
            keep = crossfade_problem(x, l, r, pool).empty() && lu.second == 0 &&
                   ru.first == 0 && lu.first + before <= l->length &&
                   after + ru.second <= r->length;
        }
        if (!keep) {
            // A removed region has no fades left to fit, and looking for it
            // would scan the whole playlist.
            if (!has(gone, x.left)) t.add_end(x.left);
            if (!has(gone, x.right)) t.add_end(x.right);
            continue;
        }
        if (named_left) used[slot(named, x.left)].second = before;
        if (named_right) used[slot(named, x.right)].first = after;
        p.crossfades[out++] = x;
    }
    p.crossfades.resize(out);
}

void fit_fades_near(Playlist& p, const Touched& t) {
    if (p.crossfades.empty()) return;
    const std::vector<std::size_t> near = near_indices(p, t);
    const Ids ids = ids_at(p, near);
    struct Reach {
        Frames in = -1;    // -1: no crossfade at that end
        Frames out = -1;
    };
    std::vector<Reach> reach(ids.size());
    for (const Crossfade& x : p.crossfades) {
        Frames before = 0, after = 0;
        crossfade_extent(x, before, after);
        if (has(ids, x.left)) reach[slot(ids, x.left)].out = before;
        if (has(ids, x.right)) reach[slot(ids, x.right)].in = after;
    }
    // fit_fades_to_crossfades(), region by region.
    for (const std::size_t i : near) {
        Region& r = p.regions[i];
        const Reach& x = reach[slot(ids, r.id)];
        if (x.in >= 0 && x.out < 0 && x.in <= r.length &&
            r.fade_out.length > r.length - x.in) {
            r.fade_out.length = r.length - x.in;
        } else if (x.out >= 0 && x.in < 0 && x.out <= r.length &&
                   r.fade_in.length > r.length - x.out) {
            r.fade_in.length = r.length - x.out;
        }
    }
}

void insert_crossfade(Playlist& p, const Crossfade& x, std::size_t left_index) {
    // The list is in region order, so one walk over the regions up to the
    // left one counts the crossfades that go first, with no index of ids.
    std::size_t k = 0;
    const std::size_t c = p.crossfades.size();
    for (std::size_t i = 0; i <= left_index && i < p.regions.size() && k < c; ++i) {
        while (k < c && p.crossfades[k].left == p.regions[i].id) ++k;
    }
    p.crossfades.insert(p.crossfades.begin() + static_cast<std::ptrdiff_t>(k), x);
}

std::string playlist_problem_near(const Playlist& p, const SourcePool& pool,
                                  std::uint16_t track_channels, const Touched& t,
                                  std::string& where) {
    where.clear();
    const std::vector<std::size_t> near = near_indices(p, t);
    // The same three passes as playlist_problem(), in its order, so the first
    // problem found is the one it would report.
    for (const std::size_t i : near) {
        std::string why = region_problem(p.regions[i], pool, track_channels);
        if (why.empty()) why = order_problem(p, i);
        if (!why.empty()) {
            where = "regions[" + std::to_string(i) + "]";
            return why;
        }
    }

    Ids watched = ids_at(p, near);
    const Ids near_ids = watched;
    watched.insert(watched.end(), t.gone().begin(), t.gone().end());
    sort_unique(watched);
    struct Ends {
        bool has_in = false, has_out = false;
        Frames in_after = 0, out_before = 0;
    };
    std::vector<Ends> ends(near_ids.size());
    const auto ends_of = [&](Id id) -> Ends* {
        return has(near_ids, id) ? &ends[slot(near_ids, id)] : nullptr;
    };
    std::size_t previous_left = kNoIndex;   // the left index of crossfades[k - 1], if known
    for (std::size_t k = 0; k < p.crossfades.size(); ++k) {
        const Crossfade& x = p.crossfades[k];
        if (!has(watched, x.left) && !has(watched, x.right)) {
            previous_left = kNoIndex;
            continue;
        }
        where = "crossfades[" + std::to_string(k) + "]";
        const std::size_t li = find_index(p, x.left, t);
        const std::size_t ri = find_index(p, x.right, t);
        std::string why = crossfade_problem(x, at(p, li), at(p, ri), pool);
        if (!why.empty()) return why;
        Ends* l = ends_of(x.left);
        Ends* r = ends_of(x.right);
        // A second crossfade on a region outside `near` would name a region
        // in it too (crossfades join neighbours), and is caught from there.
        if ((l != nullptr && l->has_out) || (r != nullptr && r->has_in)) {
            return second_crossfade_problem(x);
        }
        if (k > 0) {
            if (previous_left == kNoIndex) {
                previous_left = find_index(p, p.crossfades[k - 1].left, t);
            }
            if (previous_left != kNoIndex && previous_left > li) return kCrossfadesUnsorted;
        }
        previous_left = li;
        Frames before = 0, after = 0;
        crossfade_extent(x, before, after);
        if (l != nullptr) {
            l->has_out = true;
            l->out_before = before;
        }
        if (r != nullptr) {
            r->has_in = true;
            r->in_after = after;
        }
    }

    for (const std::size_t i : near) {
        const Region& r = p.regions[i];
        const Ends& e = ends[slot(near_ids, r.id)];
        std::string why = heard_problem(r, e.has_in, e.in_after, e.has_out, e.out_before);
        if (!why.empty()) {
            where = "regions[" + std::to_string(i) + "]";
            return why;
        }
    }
    where.clear();
    return {};
}

}  // namespace sonora::model::detail
