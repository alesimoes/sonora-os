// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/model/session_template.h"

#include <memory>
#include <utility>

namespace sonora::model {

namespace {

// The active playlist, emptied. A track always has one (check_invariants);
// the first stands in should a document ever arrive without, so the result
// still names a playlist it holds.
template <typename P>
std::vector<std::shared_ptr<const P>> active_only(const std::vector<std::shared_ptr<const P>>& lists,
                                                  Id active) {
    if (lists.empty()) return {};
    const P* keep = lists.front().get();
    for (const auto& p : lists)
        if (p->id == active) keep = p.get();
    auto empty = std::make_shared<P>();
    empty->id = keep->id;
    empty->name = keep->name;
    return {std::move(empty)};
}

}  // namespace

Snapshot template_of(const Snapshot& session, const TemplateOptions& options) {
    auto edit = std::make_shared<EditState>();
    edit->buses = session.edit->buses;
    edit->master_automation = session.edit->master_automation;
    edit->markers = options.markers && session.edit->markers ? session.edit->markers
                                                             : std::make_shared<const MarkerList>();
    edit->tracks.reserve(session.edit->tracks.size());
    for (const auto& t : session.edit->tracks) {
        auto track = std::make_shared<TrackEdit>(*t);
        track->playlists = active_only(t->playlists, t->active_playlist);
        track->midi_playlists = active_only(t->midi_playlists, t->active_playlist);
        if (!track->playlists.empty()) track->active_playlist = track->playlists.front()->id;
        if (!track->midi_playlists.empty()) track->active_playlist = track->midi_playlists.front()->id;
        edit->tracks.push_back(std::move(track));
    }

    Snapshot out;
    out.edit = std::move(edit);
    out.sources = std::make_shared<const SourcePool>();
    out.mix = session.mix;
    out.settings = session.settings;
    out.settings.session_id.clear();
    out.settings.left_takes.clear();
    out.revision = 0;
    out.next_id = session.next_id;
    return out;
}

Snapshot session_from_template(const Snapshot& tmpl, const std::string& name,
                               const std::string& session_id) {
    TemplateOptions keep;
    keep.markers = true;
    Snapshot out = template_of(tmpl, keep);
    out.settings.name = name;
    out.settings.session_id = session_id;
    return out;
}

}  // namespace sonora::model
