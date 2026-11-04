// SPDX-License-Identifier: GPL-3.0-or-later
#include "comping_demo.h"

#include "sonora/model/operations.h"

namespace sonora::ui {

using namespace model;

bool add_demo_takes(TimelineDocument& doc, int row, int from_bar, int bars, std::string& why) {
    const auto& tracks = doc.snapshot().edit->tracks;
    if (row < 0 || static_cast<std::size_t>(row) >= tracks.size()) {
        why = "there is no such track";
        return false;
    }
    const Id track = tracks[static_cast<std::size_t>(row)]->id;
    const std::string name = tracks[static_cast<std::size_t>(row)]->name;
    const Frames bar = static_cast<Frames>(doc.snapshot().settings.sample_rate) * 2;  // 120 bpm, 4/4
    for (int take = 2; take <= 3; ++take) {
        AddSource src;
        src.source.id = doc.reserve_id();
        src.source.path = "Audio Files/" + name + "_0" + std::to_string(take) + ".wav";
        src.source.length = bar * 40;
        src.source.take = take;
        src.source.recorded_on_track = track;
        if (!doc.submit(src, why)) return false;

        const std::string label = name + " take " + std::to_string(take);
        const Id playlist = doc.reserve_id();
        if (!doc.submit(AddPlaylist{track, playlist, label}, why)) return false;

        PlaceRegion place;
        place.playlist = playlist;
        place.region.id = doc.reserve_id();
        place.region.name = label;
        place.region.position = from_bar * bar;
        place.region.length = bars * bar;
        place.region.source_offset = from_bar * bar;
        place.region.channels = {{src.source.id, 0}};
        if (!doc.submit(place, why)) return false;
    }
    return true;
}

}  // namespace sonora::ui
