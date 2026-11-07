// SPDX-License-Identifier: GPL-3.0-or-later
//
// apply() for the family TrackOp (ADR-0059):
// tracks: add, remove, duplicate, name, colour, order.
#include "apply_detail.h"

namespace sonora::model::apply_detail {

bool Applier::operator()(const AddTrack& op) {
    if (op.kind == TrackKind::Midi && op.channels != 0) {
        return fail("a MIDI track has no audio channels; channels must be 0");
    }
    if (op.kind != TrackKind::Midi && (op.channels < 1 || op.channels > 2)) {
        return fail("a track has 1 or 2 channels in this version");
    }
    if (!detail::valid_text(op.name) || !detail::valid_text(op.playlist_name)) {
        return fail("a track or playlist name is not valid UTF-8");
    }
    if (!fresh(op.track) || !fresh(op.playlist)) return false;
    if (op.track == op.playlist) return fail("the track and its playlist need two ids");
    TrackMix mix = op.mix;
    mix.track = op.track;
    std::string where;
    const std::string why = detail::mix_problem(mix, op.kind, op.channels, where);
    if (!why.empty()) return fail("mix." + where + ": " + why);

    auto track = std::make_shared<TrackEdit>();
    track->id = op.track;
    track->kind = op.kind;
    track->name = op.name;
    // Stored from here on, so a later move keeps it (ADR-0023, update
    // of 2026-11-02).
    track->colour = auto_colour(before_.edit->tracks.size());
    track->channels = op.channels;
    track->active_playlist = op.playlist;
    if (op.kind == TrackKind::Audio) {
        auto playlist = std::make_shared<Playlist>();
        playlist->id = op.playlist;
        playlist->name = op.playlist_name;
        track->playlists.push_back(std::move(playlist));
    } else {
        auto playlist = std::make_shared<MidiPlaylist>();
        playlist->id = op.playlist;
        playlist->name = op.playlist_name;
        track->midi_playlists.push_back(std::move(playlist));
    }
    auto edit = std::make_shared<EditState>(*before_.edit);
    edit->tracks.push_back(std::move(track));

    after_ = before_;
    after_.edit = std::move(edit);
    set_mix(after_.mix, mix);
    after_.next_id = std::max(op.track, op.playlist) + 1;
    return true;
}

bool Applier::operator()(const RemoveTrack& op) {
    const std::size_t t = locate_track(*before_.edit, op.track);
    if (t == kNone) return fail("track " + str(op.track) + " does not exist");
    auto edit = std::make_shared<EditState>(*before_.edit);
    edit->tracks.erase(edit->tracks.begin() + static_cast<std::ptrdiff_t>(t));
    // A VCA or a folder does not keep a track that is gone; undo brings
    // both back with the track (ADR-0048 §1).
    const auto drop = [&](std::vector<Id>& members) {
        members.erase(std::remove(members.begin(), members.end(), op.track), members.end());
    };
    for (Vca& v : edit->vcas) drop(v.members);
    for (Folder& f : edit->folders) drop(f.members);
    // A group of fewer than two tracks links nothing: it goes, and undo
    // brings it back with the track.
    for (Group& g : edit->groups) {
        g.members.erase(std::remove(g.members.begin(), g.members.end(), op.track),
                        g.members.end());
    }
    edit->groups.erase(
        std::remove_if(edit->groups.begin(), edit->groups.end(),
                       [](const Group& g) { return g.members.size() < 2; }),
        edit->groups.end());
    after_ = before_;
    after_.edit = std::move(edit);
    return true;
}

bool Applier::operator()(const DuplicateTrack& op) {
    const std::size_t t = locate_track(*before_.edit, op.source);
    if (t == kNone) return fail("track " + str(op.source) + " does not exist");
    if (!detail::valid_text(op.name)) return fail("name is not valid UTF-8");
    if (!fresh(op.track)) return false;
    const std::size_t count = duplicate_track_ids(before_, op);
    if (op.track > static_cast<Id>(kMaxInteger) - count) {
        return fail("id " + str(op.track) + " leaves no room for the " +
                    str(static_cast<std::uint64_t>(count)) + " ids the copy needs");
    }
    const TrackEdit& src = *before_.edit->tracks[t];
    auto copy = std::make_shared<TrackEdit>();
    copy->id = op.track;
    copy->name = op.name;
    copy->colour = src.colour;
    copy->kind = src.kind;
    copy->channels = src.channels;
    // The copy plays where its source does (ADR-0024 §2); a track has no
    // input, so its output closes no loop.
    copy->output = src.output;
    Id next = op.track + 1;
    const auto renamed = [&](const std::string& name) {
        // "Vox.02" on "Vox" is "Vox.dup1.02" on its copy.
        if (name.compare(0, src.name.size(), src.name) == 0) {
            return op.name + name.substr(src.name.size());
        }
        return name;
    };
    const auto wanted = [&](Id playlist) {
        return op.all_playlists || playlist == src.active_playlist;
    };
    for (const auto& p : src.playlists) {
        if (!wanted(p->id)) continue;
        auto c = copy_playlist(*p, renamed(p->name), next);
        if (p->id == src.active_playlist) copy->active_playlist = c->id;
        copy->playlists.push_back(std::move(c));
    }
    for (const auto& p : src.midi_playlists) {
        if (!wanted(p->id)) continue;
        auto c = copy_playlist(*p, renamed(p->name), next);
        if (p->id == src.active_playlist) copy->active_playlist = c->id;
        copy->midi_playlists.push_back(std::move(c));
    }
    std::vector<InsertMix> values;
    std::unordered_map<Id, Id> insert_copies;
    if (op.inserts) {
        for (const Insert& in : src.inserts) {
            Insert c = in;
            c.id = next++;
            insert_copies.emplace(in.id, c.id);
            copy->inserts.push_back(c);
            if (const InsertMix* v = before_.mix.find_insert(in.id)) {
                InsertMix m = *v;
                m.insert = c.id;
                values.push_back(std::move(m));
            }
        }
    }
    // The same buses: a bus is fed once per strip, and the copy is
    // another strip, so no loop or second send can follow.
    std::vector<SendMix> send_values;
    std::unordered_map<Id, Id> send_copies;
    if (op.sends) {
        for (const Send& sd : src.sends) {
            Send c = sd;
            c.id = next++;
            send_copies.emplace(sd.id, c.id);
            copy->sends.push_back(c);
            if (const SendMix* v = before_.mix.find_send(sd.id)) {
                SendMix m = *v;
                m.send = c.id;
                send_values.push_back(m);
            }
        }
    }
    // Pro Tools' Duplicate copies automation by default. A lane that
    // names no insert or send is shared, as an undo step shares it; a
    // plugin or send lane keys the copy's insert or send, and without
    // the inserts or sends it has none to key.
    if (op.automation) {
        for (const auto& l : src.automation) {
            const bool plugin = l->param == AutomationParam::Plugin;
            const bool send = l->send != 0;
            if (!plugin && !send) {
                copy->automation.push_back(l);
                continue;
            }
            const auto& copies = plugin ? insert_copies : send_copies;
            const auto c = copies.find(plugin ? l->insert : l->send);
            if (c == copies.end()) continue;
            auto lane = std::make_shared<AutomationLane>(*l);
            (plugin ? lane->insert : lane->send) = c->second;
            copy->automation.push_back(std::move(lane));
        }
        // The copies' ids follow slot order, which need not be the
        // order of the source's ids.
        std::sort(copy->automation.begin(), copy->automation.end(),
                  [](const std::shared_ptr<const AutomationLane>& a,
                     const std::shared_ptr<const AutomationLane>& b) {
                      return lane_before(*a, *b);
                  });
    }
    auto edit = std::make_shared<EditState>(*before_.edit);
    edit->tracks.insert(edit->tracks.begin() + static_cast<std::ptrdiff_t>(t) + 1,
                        std::move(copy));
    after_ = before_;
    after_.edit = std::move(edit);
    if (const TrackMix* m = before_.mix.find(op.source)) {
        TrackMix mix = *m;
        mix.track = op.track;
        // A copy of a soloed track would solo one more track than the
        // operator asked for (ADR-0023 §2).
        mix.solo = false;
        set_mix(after_.mix, mix);
    }
    for (InsertMix& v : values) set_insert_mix(after_.mix, std::move(v));
    for (const SendMix& v : send_values) set_send_mix(after_.mix, v);
    after_.next_id = next;
    return true;
}

bool Applier::operator()(const SetTrackName& op) {
    return edit_track(op.track, [&](TrackEdit& t) {
        if (!detail::valid_text(op.name)) return fail("name is not valid UTF-8");
        t.name = op.name;
        return true;
    });
}

bool Applier::operator()(const SetTrackColour& op) {
    if (op.colour >= kColours) return fail(detail::track_colour_problem());
    return edit_track(op.track, [&](TrackEdit& t) {
        t.colour = op.colour;
        return true;
    });
}

bool Applier::operator()(const ReorderTracks& op) {
    const auto& tracks = before_.edit->tracks;
    if (op.order.size() != tracks.size()) {
        return fail("the new order names " + str(static_cast<std::uint64_t>(op.order.size())) +
                    " tracks; the session has " +
                    str(static_cast<std::uint64_t>(tracks.size())));
    }
    auto edit = std::make_shared<EditState>(*before_.edit);
    std::vector<bool> taken(tracks.size(), false);
    for (std::size_t i = 0; i < op.order.size(); ++i) {
        const std::size_t t = locate_track(*before_.edit, op.order[i]);
        if (t == kNone) return fail("track " + str(op.order[i]) + " does not exist");
        if (taken[t]) return fail("track " + str(op.order[i]) + " is named twice");
        taken[t] = true;
        edit->tracks[i] = tracks[t];
    }
    after_ = before_;
    after_.edit = std::move(edit);
    return true;
}

}  // namespace sonora::model::apply_detail
