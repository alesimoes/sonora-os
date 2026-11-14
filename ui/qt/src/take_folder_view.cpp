// SPDX-License-Identifier: GPL-3.0-or-later
#include "take_folder_view.h"

#include "sonora/model/comping.h"

namespace sonora::ui {

void TakeFolderView::set_expanded(model::Id track, bool on) {
    if (on) expanded_.insert(track);
    else expanded_.erase(track);
}

bool TakeFolderView::toggle(model::Id track) {
    const bool on = !expanded(track);
    set_expanded(track, on);
    return on;
}

std::vector<model::Id> TakeFolderView::lanes(const model::TrackEdit& track) {
    std::vector<model::Id> out;
    for (const auto& p : track.playlists)
        if (p && p->id != track.active_playlist) out.push_back(p->id);
    return out;
}

std::vector<FolderSegment> TakeFolderView::segments(const model::TrackEdit& track) {
    std::vector<FolderSegment> out;
    const model::Playlist* comp = active_playlist(&track);
    if (!comp) return out;
    const auto lane_ids = lanes(track);
    for (const auto& r : comp->regions) {
        FolderSegment s;
        s.start = r.position;
        s.end = r.end();
        s.origin = model::comp_origin(track, r, track.active_playlist);
        for (std::size_t i = 0; i < lane_ids.size(); ++i)
            if (lane_ids[i] == s.origin) {
                s.lane = static_cast<int>(i);
                s.colour = s.lane % kLaneColours;
            }
        out.push_back(s);
    }
    return out;
}

int TakeFolderView::rows(const model::TrackEdit& track) const {
    return expanded(track.id) ? 1 + static_cast<int>(lanes(track).size()) : 1;
}

}  // namespace sonora::ui
