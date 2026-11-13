// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <memory>

#include "take_folder_view.h"

using namespace sonora::ui;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static model::Region region(model::Id id, model::Frames pos, model::Frames len, model::Id src) {
    model::Region r;
    r.id = id; r.position = pos; r.length = len; r.source_offset = 0;
    r.channels.resize(1);
    r.channels[0].source = src;
    return r;
}

int main() {
    model::TrackEdit t;
    t.id = 1; t.channels = 1; t.active_playlist = 10;
    auto mk = [](model::Id id) { auto p = std::make_shared<model::Playlist>(); p->id = id; return p; };
    auto comp = mk(10), a = mk(11), b = mk(12);
    a->regions.push_back(region(1, 0, 100, 5));
    b->regions.push_back(region(2, 0, 100, 6));
    comp->regions.push_back(region(3, 0, 50, 5));
    // A swipe keeps the take's own timeline: the piece at 50 reads the source from 50.
    model::Region swiped = region(4, 50, 50, 6);
    swiped.source_offset = 50;
    comp->regions.push_back(swiped);
    t.playlists = {comp, a, b};

    TakeFolderView v;
    CHECK(!v.expanded(1));
    CHECK(v.rows(t) == 1);
    CHECK(v.toggle(1));
    CHECK(v.rows(t) == 3);
    v.reset();
    CHECK(!v.expanded(1));

    const auto seg = TakeFolderView::segments(t);
    CHECK(seg.size() == 2);
    if (seg.size() == 2) {
        CHECK(seg[0].origin == 11 && seg[0].lane == 0 && seg[0].colour == 0);
        CHECK(seg[1].origin == 12 && seg[1].lane == 1 && seg[1].colour == 1);
        CHECK(seg[1].start == 50 && seg[1].end == 100);
    }
    return fails ? 1 : 0;
}
