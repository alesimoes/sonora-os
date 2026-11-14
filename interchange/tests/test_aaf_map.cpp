// SPDX-License-Identifier: GPL-3.0-or-later
// AafDocument to Snapshot on hand-built documents: no libaaf, no file.
#include <cstdint>
#include <cstdio>

#include "aaf_map.h"
#include "aaf_rational.h"

using namespace sonora::interchange;
namespace model = sonora::model;

static int fails = 0;
#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);    \
            ++fails;                                                   \
        }                                                              \
    } while (0)

namespace {

const std::string kUmid(64, 'a');

AafMob source_mob(const std::string& id, std::int64_t length = 480000) {
    AafMob m;
    m.mob_id = id;
    m.name = "take";
    m.is_source = true;
    m.media_path = "Audio Files/take.wav";
    AafSlot s;
    s.edit_rate = {48000, 1};
    s.clips.push_back(AafFiller{length});
    m.slots.push_back(s);
    return m;
}

AafSourceClip clip(std::int64_t start, std::int64_t length, const std::string& id = kUmid) {
    AafSourceClip c;
    c.mob_id = id;
    c.start = start;
    c.length = length;
    return c;
}

AafDocument doc_with(std::vector<AafComponent> parts, AafRational rate = {25, 1},
                     const std::string& src_id = kUmid) {
    AafDocument d;
    d.mobs.push_back(source_mob(src_id));
    AafMob comp;
    comp.name = "Mix";
    AafSlot slot;
    slot.name = "Kick";
    slot.edit_rate = rate;
    slot.clips = std::move(parts);
    comp.slots.push_back(slot);
    d.mobs.push_back(comp);
    return d;
}

bool run(const AafDocument& d, model::Snapshot& s, ConversionReport& r, std::string& e) {
    return map_aaf(d, 48000, s, r, e);
}

const model::Playlist& playlist(const model::Snapshot& s, std::size_t track = 0) {
    return *s.edit->tracks[track]->playlists[0];
}

void test_rational() {
    std::int64_t out = 0;
    bool exact = false;
    CHECK(rescale_units(25, {25, 1}, 48000, out, exact) && out == 48000 && exact);
    CHECK(rescale_units(1, {30000, 1001}, 48000, out, exact) && out == 1602 && !exact);
    CHECK(rescale_units(-1, {30000, 1001}, 48000, out, exact) && out == -1602);
    CHECK(!rescale_units(1, {0, 1}, 48000, out, exact));
    CHECK(!rescale_units(INT64_MAX / 2, {1, 1}, 48000, out, exact));
}

void test_25fps_filler() {
    model::Snapshot s;
    ConversionReport r;
    std::string e;
    CHECK(run(doc_with({clip(0, 25), AafFiller{25}, clip(50, 25)}), s, r, e));
    CHECK(e.empty());
    CHECK(s.edit->tracks.size() == 1);
    CHECK(s.edit->tracks[0]->name == "Kick");
    const auto& pl = playlist(s);
    CHECK(pl.regions.size() == 2);
    CHECK(pl.regions[0].position == 0 && pl.regions[0].length == 48000);
    CHECK(pl.regions[1].position == 96000 && pl.regions[1].length == 48000);
    CHECK(pl.regions[1].source_offset == 96000);
    CHECK(r.approximated.empty());
    CHECK(s.sources->sources.size() == 1 && s.sources->sources[0].umid == kUmid);
    CHECK(s.sources->sources[0].kind == model::SourceKind::Imported);
}

void test_transition() {
    AafTransition t;
    t.length = 10;
    t.cut_point = 5;
    model::Snapshot s;
    ConversionReport r;
    std::string e;
    CHECK(run(doc_with({clip(0, 100), t, clip(10, 100)}), s, r, e));
    const auto& pl = playlist(s);
    CHECK(pl.regions.size() == 2);
    CHECK(pl.regions[1].position == 192000);
    CHECK(pl.crossfades.size() == 1);
    CHECK(pl.crossfades[0].length == 19200);
    CHECK(pl.crossfades[0].left == pl.regions[0].id && pl.crossfades[0].right == pl.regions[1].id);
    CHECK(pl.crossfades[0].align == model::Align::Centre);
}

void test_transition_without_handle_moves() {
    AafTransition t;
    t.length = 10;
    t.cut_point = 5;
    model::Snapshot s;
    ConversionReport r;
    std::string e;
    // The right clip starts at 0 of its source: no handle before the cut.
    CHECK(run(doc_with({clip(0, 100), t, clip(0, 100)}), s, r, e));
    const auto& pl = playlist(s);
    CHECK(pl.crossfades.size() == 1 && pl.crossfades[0].align == model::Align::Post);
    CHECK(!r.approximated.empty());
}

void test_missing_umid() {
    model::Snapshot s;
    ConversionReport r;
    std::string e;
    // The source mob's id is not a UMID, and one clip names nothing at all.
    CHECK(run(doc_with({clip(0, 25, "plain-id"), clip(0, 25, ""), clip(0, 25, "plain-id")},
                       {25, 1}, "plain-id"),
              s, r, e));
    CHECK(s.sources->sources[0].umid.size() == 64);
    bool said_umid = false;
    for (const auto& a : r.approximated) said_umid = said_umid || a.find("UMID") != std::string::npos;
    CHECK(said_umid);
    CHECK(r.dropped.size() == 1);
    const auto& pl = playlist(s);
    CHECK(pl.regions.size() == 2);
    // The unresolved clip stays a gap, so the third clip starts at 2 s.
    CHECK(pl.regions[1].position == 96000);
}

void test_overlap_refused() {
    AafTransition t;
    t.length = 300;
    t.cut_point = 150;
    model::Snapshot s;
    ConversionReport r;
    std::string e;
    CHECK(!run(doc_with({clip(0, 100), t, clip(0, 100)}), s, r, e));
    CHECK(e.find("overlapping") != std::string::npos);
}

void test_rounding_reported() {
    model::Snapshot s;
    ConversionReport r;
    std::string e;
    CHECK(run(doc_with({clip(0, 3)}, {30000, 1001}), s, r, e));
    CHECK(!r.approximated.empty());
    CHECK(playlist(s).regions[0].length == 4805);
}

void test_gain_fades_timecode_markers() {
    AafSourceClip c = clip(0, 100);
    c.gain_db = -6.0;
    c.fade_in = AafFade{5};
    c.fade_out = AafFade{10};
    AafDocument d = doc_with({c});
    d.timecode = AafTimecode{90000, 25, false};
    d.mobs[1].slots[0].markers.push_back({25, "Verse"});
    d.unmapped.push_back("volume automation on Kick");
    model::Snapshot s;
    ConversionReport r;
    std::string e;
    CHECK(run(d, s, r, e));
    const auto& reg = playlist(s).regions[0];
    CHECK(reg.gain_db == -6.0f);
    CHECK(reg.fade_in.length == 9600 && reg.fade_out.length == 19200);
    CHECK(s.settings.timecode.fps == model::Fps::F25);
    CHECK(s.settings.timecode.origin == 90000 * 1920);
    CHECK(s.edit->markers->markers.size() == 1 && s.edit->markers->markers[0].position == 48000);
    CHECK(r.dropped.size() == 1);
}

void test_no_composition() {
    AafDocument d;
    d.mobs.push_back(source_mob(kUmid));
    model::Snapshot s;
    ConversionReport r;
    std::string e;
    CHECK(!run(d, s, r, e));
    CHECK(!e.empty());
}

}  // namespace

int main() {
    test_rational();
    test_25fps_filler();
    test_transition();
    test_transition_without_handle_moves();
    test_missing_umid();
    test_overlap_refused();
    test_rounding_reported();
    test_gain_fades_timecode_markers();
    test_no_composition();
    return fails ? 1 : 0;
}
