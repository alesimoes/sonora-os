// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/model/session_template.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <utility>

#include "sonora/model/template_list.h"

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

namespace {

std::string joined(const std::string& folder, const std::string& name) {
    if (folder.empty() || folder.back() == '/') return folder + name;
    return folder + '/' + name;
}

// 0 when `dir` holds no <name>.sonora, so it may be a category; -1 when it
// holds one that is not a session file, so it is neither; 1 when listed.
int add_template(TemplateFolder& folder, const std::string& dir, const std::string& name,
                 const std::string& category, std::vector<TemplateInfo>& out) {
    TemplateInfo info;
    info.name = name;
    info.file = joined(dir, name + ".sonora");
    info.category = category;
    std::string bytes;
    if (!folder.read_file(info.file, bytes)) return 0;
    if (!read_template_version(bytes, info)) return -1;
    out.push_back(std::move(info));
    return 1;
}

}  // namespace

std::vector<TemplateInfo> list_templates(TemplateFolder& folder, const std::string& root) {
    std::vector<TemplateInfo> out;
    std::vector<std::string> top = folder.subfolders(root);
    std::sort(top.begin(), top.end());
    std::vector<TemplateInfo> categorised;
    for (const std::string& name : top) {
        const std::string dir = joined(root, name);
        if (add_template(folder, dir, name, std::string(), out) != 0) continue;
        // One level only: a folder in a category is a template or nothing.
        std::vector<std::string> inner = folder.subfolders(dir);
        std::sort(inner.begin(), inner.end());
        for (const std::string& t : inner) add_template(folder, joined(dir, t), t, name, categorised);
    }
    out.insert(out.end(), std::make_move_iterator(categorised.begin()),
               std::make_move_iterator(categorised.end()));
    return out;
}

}  // namespace sonora::model
