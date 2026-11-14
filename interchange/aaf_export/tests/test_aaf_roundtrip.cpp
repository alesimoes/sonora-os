// SPDX-License-Identifier: GPL-3.0-or-later
// Snapshot -> AafExporter -> AafDocument -> AafImporter -> Snapshot (ADR-0171).
// The neutral document is the seam: no libaaf, no file. AafExporter::write
// also copies each source file beside the .aaf, which needs real audio on
// disk; the mapping stage it runs first, map_to_aaf, is called directly.
#include <cstdio>
#include <memory>

#include "fake_aaf_reader.h"
#include "aaf_export_map.h"
#include "sonora/interchange/aaf_import.h"

using namespace sonora::interchange;
namespace model = sonora::model;

static int fails = 0;
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
            ++fails;                                                \
        }                                                           \
    } while (0)

namespace {

model::Region region(model::Id id, model::Frames pos, model::Frames len, model::Frames off) {
    model::Region r;
    r.id = id;
    r.name = "r" + std::to_string(id);
    r.position = pos;
    r.length = len;
    r.source_offset = off;
    r.channels.push_back({1, 0});
    return r;
}

model::Snapshot build() {
    model::Playlist pl;
    pl.id = 10;
    pl.regions.push_back(region(1, 0, 2000, 1000));
    pl.regions[0].gain_db = -3.0f;
    pl.regions[0].fade_in.length = 64;
    pl.regions.push_back(region(2, 2000, 1000, 5000));
    pl.regions[1].fade_out.length = 50;
    model::Crossfade x;
    x.left = 1;
    x.right = 2;
    x.length = 200;
    pl.crossfades.push_back(x);
    pl.regions.push_back(region(3, 4000, 500, 20000));  // gap before, trimmed
    pl.regions[2].gain_db = 2.0f;

    model::Snapshot s = model::new_session({});
    auto edit = std::make_shared<model::EditState>(*s.edit);
    auto t = std::make_shared<model::TrackEdit>();
    t->id = 5;
    t->name = "Kick";
    t->active_playlist = pl.id;
    t->playlists.push_back(std::make_shared<const model::Playlist>(std::move(pl)));
    model::AutomationLane lane;
    lane.points.push_back({0, 0.0f});
    lane.points.push_back({1000, 0.5f});
    t->automation.push_back(std::make_shared<const model::AutomationLane>(lane));
    edit->tracks.push_back(t);
    model::MarkerList ml;
    model::Marker m;
    m.position = 1500;
    m.name = "Verse";
    ml.markers.push_back(m);
    edit->markers = std::make_shared<const model::MarkerList>(std::move(ml));
    s.edit = edit;
    auto pool = std::make_shared<model::SourcePool>();
    model::Source src;
    src.id = 1;
    src.path = "Audio Files/kick.wav";
    src.length = 480000;
    src.umid = std::string(64, 'b');
    pool->sources.push_back(src);
    s.sources = pool;
    return s;
}

const model::Playlist& active(const model::Snapshot& s) {
    const auto& t = *s.edit->tracks.at(0);
    for (const auto& p : t.playlists) {
        if (p->id == t.active_playlist) return *p;
    }
    return *t.playlists.at(0);
}

}  // namespace

int main() {
    const model::Snapshot a = build();
    AafDocument doc;
    ConversionReport out_rep;
    std::string err;
    CHECK(map_to_aaf(a, doc, out_rep, err));

    // Automation cannot ride in AAF: it must be listed, not lost silently.
    bool auto_listed = false;
    for (const auto& l : out_rep.dropped) {
        if (l.find("automation") != std::string::npos) auto_listed = true;
    }
    CHECK(auto_listed);

    FakeAafReader reader;
    reader.doc = doc;
    AafImporter imp(&reader);
    model::Snapshot b;
    ConversionReport in_rep;
    const bool imported = imp.read("x.aaf", b, in_rep, err);
    if (!imported) std::printf("import error: %s\n", err.c_str());
    CHECK(imported);
    if (!imported) return 1;
    for (const auto& l : in_rep.dropped) std::printf("in dropped: %s\n", l.c_str());
    for (const auto& l : in_rep.approximated) std::printf("in approx: %s\n", l.c_str());
    CHECK(in_rep.dropped.empty());
    CHECK(in_rep.approximated.empty());

    CHECK(b.edit->tracks.size() == 1);
    if (b.edit->tracks.size() != 1) return 1;
    CHECK(b.edit->tracks[0]->name == "Kick");
    const model::Playlist& pa = active(a);
    const model::Playlist& pb = active(b);
    CHECK(pb.regions.size() == pa.regions.size());
    for (std::size_t i = 0; i < pa.regions.size() && i < pb.regions.size(); ++i) {
        const model::Region& x = pa.regions[i];
        const model::Region& y = pb.regions[i];
        CHECK(x.position == y.position);
        CHECK(x.length == y.length);
        CHECK(x.source_offset == y.source_offset);
        CHECK(x.gain_db == y.gain_db);
        CHECK(x.fade_in.length == y.fade_in.length);
        CHECK(x.fade_out.length == y.fade_out.length);
    }
    CHECK(pb.crossfades.size() == 1);
    if (pb.crossfades.size() == 1 && pb.regions.size() >= 2) {
        CHECK(pb.crossfades[0].length == 200);
        CHECK(pb.crossfades[0].left == pb.regions[0].id);
        CHECK(pb.crossfades[0].right == pb.regions[1].id);
    }

    CHECK(b.edit->markers && b.edit->markers->markers.size() == 1);
    if (b.edit->markers && !b.edit->markers->markers.empty()) {
        CHECK(b.edit->markers->markers[0].position == 1500);
        CHECK(b.edit->markers->markers[0].name == "Verse");
    }

    // The documented loss: automation does not come back.
    CHECK(b.edit->tracks[0]->automation.empty());
    CHECK(b.settings.sample_rate == a.settings.sample_rate);
    return fails ? 1 : 0;
}
