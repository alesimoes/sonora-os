// SPDX-License-Identifier: GPL-3.0-or-later
//
// apply() for the family PlaylistOp (ADR-0059):
// a track's playlists.
#include "apply_detail.h"

namespace sonora::model::apply_detail {

bool Applier::operator()(const AddPlaylist& op) {
    return edit_track(op.track, [&](TrackEdit& t) {
        if (!detail::valid_text(op.name)) return fail("name is not valid UTF-8");
        if (!fresh(op.playlist)) return false;
        if (t.kind == TrackKind::Audio) {
            auto p = std::make_shared<Playlist>();
            p->id = op.playlist;
            p->name = op.name;
            if (op.freeze) {
                const bool from_here =
                    std::any_of(t.playlists.begin(), t.playlists.end(),
                                [&](const auto& q) { return q->id == op.freeze->from; });
                if (!from_here) return fail("a frozen playlist is made from a playlist of its track");
                for (const Id i : op.freeze->inserts) {
                    const bool on_track =
                        std::any_of(t.inserts.begin(), t.inserts.end(),
                                    [&](const Insert& in) { return in.id == i; });
                    if (!on_track) return fail("a frozen playlist names an insert that is not on its track");
                }
                p->freeze = op.freeze;
            }
            t.playlists.push_back(std::move(p));
        } else {
            auto p = std::make_shared<MidiPlaylist>();
            p->id = op.playlist;
            p->name = op.name;
            t.midi_playlists.push_back(std::move(p));
        }
        next_id_ = op.playlist + 1;
        return true;
    });
}

bool Applier::operator()(const SetActivePlaylist& op) {
    return edit_track(op.track, [&](TrackEdit& t) {
        const auto named = [&](const auto& list) {
            return std::any_of(list.begin(), list.end(),
                               [&](const auto& p) { return p->id == op.playlist; });
        };
        if (named(t.playlists) || named(t.midi_playlists)) {
            t.active_playlist = op.playlist;
            return true;
        }
        return fail("playlist " + str(op.playlist) + " is not on track " +
                    str(op.track));
    });
}

bool Applier::operator()(const DuplicatePlaylist& op) {
    if (!detail::valid_text(op.name)) return fail("name is not valid UTF-8");
    if (!fresh(op.playlist)) return false;
    const std::size_t count = duplicate_playlist_ids(before_, op);
    if (count == 0) {
        return fail("playlist " + str(op.source) + " is not on track " + str(op.track));
    }
    if (op.playlist > static_cast<Id>(kMaxInteger) - count) {
        return fail("id " + str(op.playlist) + " leaves no room for the " +
                    str(static_cast<std::uint64_t>(count)) + " ids the copy needs");
    }
    Id next = op.playlist;
    const bool ok = edit_track(op.track, [&](TrackEdit& t) {
        for (const auto& p : t.playlists) {
            if (p->id != op.source) continue;
            t.playlists.push_back(copy_playlist(*p, op.name, next));
            return true;
        }
        for (const auto& p : t.midi_playlists) {
            if (p->id != op.source) continue;
            t.midi_playlists.push_back(copy_playlist(*p, op.name, next));
            return true;
        }
        return false;
    });
    if (ok) after_.next_id = next;
    return ok;
}

bool Applier::operator()(const SetPlaylistName& op) {
    return edit_track(op.track, [&](TrackEdit& t) {
        if (!detail::valid_text(op.name)) return fail("name is not valid UTF-8");
        if (op.name.empty()) return fail("a playlist needs a name");
        const auto rename = [&](auto& list) {
            for (auto& p : list) {
                if (p->id != op.playlist) continue;
                using Kind = std::remove_const_t<
                    typename std::decay_t<decltype(list)>::value_type::element_type>;
                auto c = std::make_shared<Kind>(*p);
                c->name = op.name;
                p = std::move(c);
                return true;
            }
            return false;
        };
        if (rename(t.playlists) || rename(t.midi_playlists)) return true;
        return fail("playlist " + str(op.playlist) + " is not on track " + str(op.track));
    });
}

bool Applier::operator()(const RemovePlaylist& op) {
    return edit_track(op.track, [&](TrackEdit& t) {
        if (op.playlist == t.active_playlist) {
            return fail("playlist " + str(op.playlist) +
                        " is the track's active playlist; choose another first");
        }
        const auto remove = [&](auto& list) {
            for (auto it = list.begin(); it != list.end(); ++it) {
                if ((*it)->id != op.playlist) continue;
                list.erase(it);
                return true;
            }
            return false;
        };
        if (remove(t.playlists) || remove(t.midi_playlists)) return true;
        return fail("playlist " + str(op.playlist) + " is not on track " + str(op.track));
    });
}

}  // namespace sonora::model::apply_detail
