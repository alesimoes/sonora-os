// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>

#include "../src/ptx_map.h"

using namespace sonora::interchange;

static int fails = 0;
#define CHECK(c)                                             \
    do {                                                     \
        if (!(c)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #c); \
            ++fails;                                         \
        }                                                    \
    } while (0)

namespace {

PtxDocument base_doc() {
    PtxDocument d;
    d.sample_rate = 48000;
    d.sources.push_back({"kick", "C:\\Sessions\\Audio Files\\kick.wav", 100000});
    PtxTrack t;
    t.name = "Kick";
    PtxRegion a;
    a.length_samples = 1000;
    PtxRegion b = a;
    b.start_samples = 1000;
    b.gain = 0.5f;
    b.muted = true;
    t.regions = {b, a};
    d.tracks.push_back(t);
    return d;
}

struct Run {
    sonora::model::Snapshot s;
    ConversionReport rep;
    std::string err;
    bool ok;
};

Run run(const PtxDocument& d, std::uint32_t rate = 0) {
    Run r;
    r.ok = map_ptx_document(d, rate, r.s, r.rep, r.err);
    return r;
}

}  // namespace

int main() {
    {
        PtxDocument d;  // empty but with a rate: an empty session
        d.sample_rate = 44100;
        Run r = run(d);
        CHECK(r.ok);
        CHECK(r.s.settings.sample_rate == 44100);
        CHECK(r.s.edit->tracks.empty());
        std::string why;
        CHECK(sonora::model::check_invariants(r.s, why));
    }
    {
        PtxDocument d;  // no rate
        CHECK(!run(d).ok);
    }
    {
        Run r = run(base_doc());
        CHECK(r.ok);
        CHECK(r.s.sources->sources.size() == 1);
        CHECK(r.s.sources->sources[0].path == "Audio Files/kick.wav");
        CHECK(r.s.edit->tracks.size() == 1);
        const auto& pl = *r.s.edit->tracks[0]->playlists[0];
        CHECK(pl.regions.size() == 2);
        CHECK(pl.regions[0].position == 0);  // sorted
        CHECK(!pl.regions[0].mute && pl.regions[1].mute);
        CHECK(pl.regions[1].gain_db < -5.9f && pl.regions[1].gain_db > -6.1f);
        std::string why;
        CHECK(sonora::model::check_invariants(r.s, why));
    }
    {
        PtxDocument d = base_doc();
        d.tracks[0].regions[1].length_samples = 1500;  // overlaps the one at 1000
        Run r = run(d);
        CHECK(!r.ok);
        CHECK(r.err.find("overlap") != std::string::npos);
    }
    {
        PtxDocument d = base_doc();
        d.tracks[0].regions[0].source_index = 3;
        Run r = run(d);
        CHECK(!r.ok);
        CHECK(r.err.find("source") != std::string::npos);
    }
    {
        PtxDocument d = base_doc();
        d.unsupported = {"plug-in inserts", "automation"};
        Run r = run(d);
        CHECK(r.ok);
        CHECK(r.rep.dropped.size() == 2);
    }
    {
        PtxDocument d = base_doc();
        d.tracks[0].regions[0].gain = 0.0f;  // silent: limited to the floor
        Run r = run(d);
        CHECK(r.ok);
        CHECK(!r.rep.approximated.empty());
    }
    {
        Run r = run(base_doc(), 96000);  // rate mismatch
        CHECK(r.ok);
        CHECK(r.s.settings.sample_rate == 96000);
        CHECK(!r.rep.approximated.empty());
        CHECK(r.s.sources->sources[0].length == 200000);
        CHECK(r.s.edit->tracks[0]->playlists[0]->regions[1].position == 2000);
    }
    if (fails == 0) std::printf("test_ptx_map: ok\n");
    return fails == 0 ? 0 : 1;
}
