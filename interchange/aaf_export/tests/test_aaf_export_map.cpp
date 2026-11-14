// SPDX-License-Identifier: GPL-3.0-or-later
// Snapshot to AafDocument on hand-built sessions: no libaaf, no file.
#include <cstdio>
#include <memory>

#include "aaf_export_map.h"

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

const std::string kUmid(64, 'b');

model::Region region(model::Id id, model::Frames pos, model::Frames len, model::Id source = 1) {
    model::Region r;
    r.id = id;
    r.name = "r" + std::to_string(id);
    r.position = pos;
    r.length = len;
    r.channels.push_back({source, 0});
    return r;
}

// One mono track "Kick" holding `pl`, over one source (UMID `umid`).
model::Snapshot session_with(model::Playlist pl, const std::string& umid = kUmid) {
    model::Snapshot s = model::new_session({});
    auto edit = std::make_shared<model::EditState>(*s.edit);
    pl.id = 10;
    auto t = std::make_shared<model::TrackEdit>();
    t->id = 5;
    t->name = "Kick";
    t->active_playlist = pl.id;
    t->playlists.push_back(std::make_shared<const model::Playlist>(std::move(pl)));
    edit->tracks.push_back(t);
    s.edit = edit;
    auto pool = std::make_shared<model::SourcePool>();
    model::Source src;
    src.id = 1;
    src.path = "Audio Files/kick.wav";
    src.length = 480000;
    src.umid = umid;
    pool->sources.push_back(src);
    s.sources = pool;
    return s;
}

const AafMob* composition(const AafDocument& d) {
    for (const AafMob& m : d.mobs) {
        if (!m.is_source) return &m;
    }
    return nullptr;
}

bool mapped(const model::Snapshot& s, AafDocument& d, ConversionReport& rep) {
    std::string err;
    const bool ok = map_to_aaf(s, d, rep, err);
    if (!ok) std::printf("error: %s\n", err.c_str());
    return ok;
}

void test_empty() {
    AafDocument d;
    ConversionReport rep;
    CHECK(mapped(model::new_session({}), d, rep));
    CHECK(d.mobs.size() == 1);
    CHECK(composition(d) && composition(d)->slots.empty());
    CHECK(rep.dropped.empty());
}

void test_gap_and_clip() {
    model::Playlist pl;
    pl.regions.push_back(region(1, 1000, 2000));
    pl.regions.back().source_offset = 300;
    pl.regions.back().gain_db = -3.0f;
    pl.regions.back().fade_in.length = 64;
    pl.regions.push_back(region(2, 5000, 100));
    AafDocument d;
    ConversionReport rep;
    CHECK(mapped(session_with(pl), d, rep));
    CHECK(d.mobs.size() == 2);
    const AafSlot& slot = composition(d)->slots.at(0);
    CHECK(slot.edit_rate.num == 48000 && slot.edit_rate.den == 1);
    CHECK(slot.clips.size() == 4);
    CHECK(std::get<AafFiller>(slot.clips[0]).length == 1000);
    const auto& c = std::get<AafSourceClip>(slot.clips[1]);
    CHECK(c.mob_id == kUmid && c.start == 300 && c.length == 2000);
    CHECK(c.gain_db && *c.gain_db == -3.0);
    CHECK(c.fade_in && c.fade_in->length == 64 && !c.fade_out);
    CHECK(std::get<AafFiller>(slot.clips[2]).length == 2000);
    CHECK(rep.approximated.empty());
}

void test_crossfade() {
    model::Playlist pl;
    pl.regions.push_back(region(1, 0, 1000));
    pl.regions.push_back(region(2, 1000, 1000));
    model::Crossfade x;
    x.left = 1;
    x.right = 2;
    x.length = 200;
    pl.crossfades.push_back(x);
    AafDocument d;
    ConversionReport rep;
    CHECK(mapped(session_with(pl), d, rep));
    const AafSlot& slot = composition(d)->slots.at(0);
    CHECK(slot.clips.size() == 3);
    const auto* t = std::get_if<AafTransition>(&slot.clips[1]);
    CHECK(t && t->length == 200 && t->cut_point == 100);
}

void test_timecode_25() {
    model::Snapshot s = model::new_session({});
    s.settings.timecode.fps = model::Fps::F25;
    s.settings.timecode.origin = 172'800'000;  // 01:00:00:00 at 48 kHz
    AafDocument d;
    ConversionReport rep;
    CHECK(mapped(s, d, rep));
    CHECK(d.timecode && d.timecode->fps == 25 && !d.timecode->drop);
    CHECK(d.timecode->start == 90000);
    CHECK(rep.approximated.empty());
}

void test_timecode_drop() {
    model::Snapshot s = model::new_session({});
    s.settings.timecode.fps = model::Fps::F29_97df;
    s.settings.timecode.origin = 48000 * 1001;  // exactly 30000 frames
    AafDocument d;
    ConversionReport rep;
    CHECK(mapped(s, d, rep));
    CHECK(d.timecode && d.timecode->fps == 30 && d.timecode->drop);
    CHECK(d.timecode->start == 30000);
}

void test_missing_umid() {
    model::Playlist pl;
    pl.regions.push_back(region(1, 0, 1000));
    AafDocument d;
    ConversionReport rep;
    CHECK(mapped(session_with(pl, ""), d, rep));
    const auto& c = std::get<AafSourceClip>(composition(d)->slots.at(0).clips.at(0));
    CHECK(c.mob_id.size() == 64);
    CHECK(rep.approximated.size() == 1);
    CHECK(rep.approximated[0].find("no UMID") != std::string::npos);
    AafDocument d2;
    ConversionReport rep2;
    CHECK(mapped(session_with(pl, ""), d2, rep2));
    CHECK(std::get<AafSourceClip>(composition(d2)->slots.at(0).clips.at(0)).mob_id == c.mob_id);
}

void test_dropped() {
    model::Playlist pl;
    pl.regions.push_back(region(1, 0, 1000));
    model::Snapshot s = session_with(pl);
    auto edit = std::make_shared<model::EditState>(*s.edit);
    auto t = std::make_shared<model::TrackEdit>(*edit->tracks[0]);
    model::AutomationLane lane;
    lane.points.push_back({0, 0.0f});
    t->automation.push_back(std::make_shared<const model::AutomationLane>(lane));
    model::Insert ins;
    ins.plugin.name = "Comp";
    t->inserts.push_back(ins);
    edit->tracks[0] = t;
    s.edit = edit;
    AafDocument d;
    ConversionReport rep;
    CHECK(mapped(s, d, rep));
    bool auto_seen = false, plug_seen = false;
    for (const auto& l : rep.dropped) {
        if (l.find("automation") != std::string::npos) auto_seen = true;
        if (l.find("plugin") != std::string::npos) plug_seen = true;
    }
    CHECK(auto_seen && plug_seen);
}

}  // namespace

int main() {
    test_empty();
    test_gap_and_clip();
    test_crossfade();
    test_timecode_25();
    test_timecode_drop();
    test_missing_umid();
    test_dropped();
    return fails ? 1 : 0;
}
