// SPDX-License-Identifier: GPL-3.0-or-later
// OmfDocument to Snapshot on hand-built documents: no file, no reader.
#include <cstdint>
#include <cstdio>

#include "omf_map.h"

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

OmfClip clip(std::int64_t start, std::int64_t length, int source = 0) {
    OmfClip c;
    c.source = source;
    c.start = start;
    c.length = length;
    return c;
}

OmfDocument doc_with(std::vector<OmfClip> clips, std::int64_t rate = 48000) {
    OmfDocument d;
    d.sample_rate = rate;
    OmfMediaSource s;
    s.name = "take";
    s.path = "Audio Files/take.wav";
    s.length = 480000;
    d.sources.push_back(s);
    OmfTrack t;
    t.name = "Kick";
    t.clips = std::move(clips);
    d.tracks.push_back(t);
    return d;
}

bool run(const OmfDocument& d, model::Snapshot& s, ConversionReport& r, std::string& e) {
    return map_omf(d, 48000, s, r, e);
}

void test_basic() {
    OmfClip a = clip(0, 1000);
    a.gain_db = -6.0;
    a.fade_in = 100;
    a.fade_out = 200;
    OmfClip b = clip(2000, 500);
    b.mute = true;
    b.source_offset = 300;
    model::Snapshot s;
    ConversionReport r;
    std::string e;
    CHECK(run(doc_with({b, a}), s, r, e));
    CHECK(s.edit->tracks.size() == 1 && s.edit->tracks[0]->name == "Kick");
    const auto& pl = *s.edit->tracks[0]->playlists[0];
    CHECK(pl.regions.size() == 2);
    CHECK(pl.regions[0].position == 0 && pl.regions[0].length == 1000);
    CHECK(pl.regions[0].gain_db == -6.0f);
    CHECK(pl.regions[0].fade_in.length == 100 && pl.regions[0].fade_out.length == 200);
    CHECK(pl.regions[1].mute && pl.regions[1].source_offset == 300);
    CHECK(s.sources->sources.size() == 1);
    CHECK(r.dropped.empty() && r.approximated.empty());
}

void test_empty() {
    OmfDocument d;
    d.sample_rate = 48000;
    model::Snapshot s;
    ConversionReport r;
    std::string e;
    CHECK(!run(d, s, r, e) && !e.empty());
    e.clear();
    CHECK(run(doc_with({}), s, r, e));
    CHECK(s.edit->tracks[0]->playlists[0]->regions.empty());
}

void test_overlap_refused() {
    model::Snapshot s;
    ConversionReport r;
    std::string e;
    CHECK(!run(doc_with({clip(0, 1000), clip(500, 1000)}), s, r, e));
    CHECK(e.find("overlap") != std::string::npos);
}

void test_missing_source() {
    model::Snapshot s;
    ConversionReport r;
    std::string e;
    CHECK(run(doc_with({clip(0, 100, 5), clip(200, 100)}), s, r, e));
    CHECK(s.edit->tracks[0]->playlists[0]->regions.size() == 1);
    CHECK(r.dropped.size() == 1);
}

void test_unsupported() {
    OmfDocument d = doc_with({clip(0, 100)});
    d.unsupported = {"pan automation", "reverb effect"};
    model::Snapshot s;
    ConversionReport r;
    std::string e;
    CHECK(run(d, s, r, e));
    CHECK(r.dropped.size() == 2);
}

void test_rate_mismatch() {
    model::Snapshot s;
    ConversionReport r;
    std::string e;
    CHECK(run(doc_with({clip(0, 44100)}, 44100), s, r, e));
    CHECK(s.edit->tracks[0]->playlists[0]->regions[0].length == 48000);
    CHECK(!r.approximated.empty());
    r = ConversionReport();
    CHECK(run(doc_with({clip(1, 1)}, 44100), s, r, e));
    CHECK(s.edit->tracks[0]->playlists[0]->regions[0].position == 1);
    CHECK(r.approximated.size() >= 2);
}

}  // namespace

int main() {
    test_basic();
    test_empty();
    test_overlap_refused();
    test_missing_source();
    test_unsupported();
    test_rate_mismatch();
    if (fails == 0) std::printf("omf_map: ok\n");
    return fails == 0 ? 0 : 1;
}
