// SPDX-License-Identifier: GPL-3.0-or-later
//
// Consolidate, the engine's half (ADR-0031 §6): the rendered file equals the
// range as playback mixes it (hand-computed here), is never written over
// another, and goes into the session as one undo step, refused while a take
// is open.
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "sonora/closed_take.h"
#include "sonora/consolidate.h"
#include "sonora/model/operations.h"
#include "sonora/session.h"
#include "sonora/wav_reader.h"
#include "sonora/wav_writer.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

using namespace sonora;
namespace fs = std::filesystem;

SessionConfig config(const char* dir) {
    SessionConfig cfg;
    cfg.dir = dir;
    cfg.track_count = 1;
    cfg.ring_seconds = 0.01;
    cfg.format = WavWriter::Format::Float32;
    return cfg;
}

// A constant `value` for `frames` frames, `channels` wide.
bool write_constant(const std::string& path, std::uint32_t rate, std::uint16_t channels,
                    std::size_t frames, float value) {
    WavWriter w;
    if (!w.open(path, rate, channels, WavWriter::Format::Float32, 0)) return false;
    const std::vector<float> block(frames * channels, value);
    return w.write(block.data(), frames) && w.close();
}

struct Rig {
    SessionConfig cfg;
    std::unique_ptr<Session> session;
    std::unique_ptr<model::Document> doc;
    model::Id track = 0, playlist = 0;
    std::uint16_t channels = 0;
};

// Source 1 holds 1.0, source 2 holds 0.5, both 1000 frames. Region A plays
// source 1 over [0, 200), region B source 2 over [200, 400) from its frame
// 100, so it has handle; a linear crossfade of 100 frames joins them,
// centred on the cut at 200.
bool make_rig(Rig& r, const char* dir) {
    fs::remove_all(dir);
    r.cfg = config(dir);
    r.session = std::make_unique<Session>(r.cfg);
    if (!r.session->create_dirs()) return false;
    r.doc = std::make_unique<model::Document>(engine_document(*r.session));
    const model::TrackEdit& t = *r.doc->current().edit->tracks[0];
    r.track = t.id;
    r.playlist = t.active_playlist;
    r.channels = t.channels;
    const std::uint32_t rate = r.doc->current().settings.sample_rate;
    if (!write_constant(r.cfg.dir + "/Audio Files/a.wav", rate, r.channels, 1000, 1.0f)) return false;
    if (!write_constant(r.cfg.dir + "/Audio Files/b.wav", rate, r.channels, 1000, 0.5f)) return false;

    std::vector<model::ReplicationUpdate> updates;
    std::string err;
    const auto apply = [&](const model::Operation& op) {
        model::ReplicationUpdate u;
        return r.doc->apply(op, u, err);
    };
    model::Id next = r.doc->current().next_id;
    const model::Id ids[2] = {next, next + 1};
    const char* names[2] = {"Audio Files/a.wav", "Audio Files/b.wav"};
    for (int i = 0; i < 2; ++i) {
        model::AddSource add;
        add.source.id = ids[i];
        add.source.path = names[i];
        add.source.channels = r.channels;
        add.source.length = 1000;
        add.source.format = model::AudioFormat::Float32;
        if (!apply(add)) return false;
    }
    next = r.doc->current().next_id;
    const auto region = [&](model::Id id, model::Id src, model::Frames pos,
                            model::Frames offset) {
        model::PlaceRegion p;
        p.playlist = r.playlist;
        p.region.id = id;
        p.region.name = "r" + std::to_string(id);
        p.region.position = pos;
        p.region.length = 200;
        p.region.source_offset = offset;
        for (std::uint16_t c = 0; c < r.channels; ++c) p.region.channels.push_back({src, c});
        p.split_right_id = id + 100;
        return p;
    };
    if (!apply(region(next, ids[0], 0, 0))) return false;
    if (!apply(region(next + 1, ids[1], 200, 100))) return false;
    model::SetCrossfade x;
    x.playlist = r.playlist;
    x.crossfade.left = next;
    x.crossfade.right = next + 1;
    x.crossfade.length = 100;
    x.crossfade.shape = model::FadeShape::Linear;
    return apply(x);
}

const model::Playlist* playlist_of(const model::Snapshot& s, model::Id id) {
    for (const auto& t : s.edit->tracks) {
        for (const auto& p : t->playlists) {
            if (p->id == id) return p.get();
        }
    }
    return nullptr;
}

std::vector<float> read_all(const std::string& path, std::uint16_t& channels) {
    WavReader rd;
    std::string e;
    if (!rd.open(path, e)) return {};
    channels = rd.channels();
    std::vector<float> v(static_cast<std::size_t>(rd.frames()) * rd.channels());
    if (rd.read(0, rd.frames(), v.data()) != rd.frames()) return {};
    return v;
}

int test_two_takes_with_a_crossfade() {
    Rig r;
    CHECK(make_rig(r, "test_consolidate_xfade"));
    const std::size_t undo_before = r.doc->history().undo_depth();
    double last = 0;
    int calls = 0;
    ConsolidateHooks hooks;
    hooks.progress = [&](double f) { last = f; ++calls; };
    std::vector<model::ReplicationUpdate> updates;
    std::string err;
    ConsolidateRequest rq;
    rq.track = r.track;
    rq.start = 100;
    rq.end = 350;
    const bool done = consolidate(*r.doc, *r.session, r.cfg.dir, rq, hooks, updates, err);
    if (!done) std::fprintf(stderr, "  refused: %s\n", err.c_str());
    CHECK(done);
    CHECK(err.empty());
    CHECK(updates.size() == 1);
    CHECK(calls >= 1 && last == 1.0);
    CHECK(r.doc->history().undo_depth() == undo_before + 1);

    // The file is the sum, computed by hand: 1.0 up to the crossfade, then
    // (1 - x) * 1.0 + x * 0.5 with x running 0..1 over [150, 250), then 0.5.
    std::uint16_t ch = 0;
    const std::vector<float> got = read_all(r.cfg.dir + "/Audio Files/Audio 1_cons_01.wav", ch);
    CHECK(ch == r.channels && got.size() == 250u * ch);
    for (int i = 0; i < 250; ++i) {
        const int t = 100 + i;
        float want = 1.0f;
        if (t >= 250) {
            want = 0.5f;
        } else if (t >= 150) {
            const float x = static_cast<float>(t - 150) / 100.0f;
            want = (1.0f - x) * 1.0f + x * 0.5f;
        }
        for (int c = 0; c < ch; ++c) {
            if (std::fabs(got[static_cast<std::size_t>(i) * ch + c] - want) > 1e-5f) {
                std::fprintf(stderr, "frame %d: %f want %f\n", t,
                             got[static_cast<std::size_t>(i) * ch + c], want);
                return 1;
            }
        }
    }

    // The range is now one region of the rendered file; the ends remain.
    const model::Playlist* p = playlist_of(r.doc->current(), r.playlist);
    CHECK(p != nullptr && p->regions.size() == 3);
    CHECK(p->regions[1].position == 100 && p->regions[1].length == 250);
    CHECK(p->regions[1].source_offset == 0);
    const model::Source* src = r.doc->current().sources->find(p->regions[1].channels[0].source);
    CHECK(src != nullptr && src->path == "Audio Files/Audio 1_cons_01.wav");
    CHECK(p->regions[0].end() == 100 && p->regions[2].position == 350);

    // One undo step takes the region away again.
    model::ReplicationUpdate u;
    CHECK(r.doc->undo(u, err));
    p = playlist_of(r.doc->current(), r.playlist);
    CHECK(p->regions.size() == 2 && p->crossfades.size() == 1);
    CHECK(r.doc->history().undo_depth() == undo_before);
    return 0;
}

int test_never_overwrites_and_cancel() {
    Rig r;
    CHECK(make_rig(r, "test_consolidate_names"));
    const std::string taken = r.cfg.dir + "/Audio Files/Audio 1_cons_01.wav";
    {
        std::FILE* f = std::fopen(taken.c_str(), "wb");
        CHECK(f != nullptr);
        std::fputs("keep", f);
        std::fclose(f);
    }
    ConsolidateRequest rq;
    rq.track = r.track;
    rq.start = 0;
    rq.end = 400;
    std::vector<model::ReplicationUpdate> updates;
    std::string err;

    // Cancelled: no file, no change, no update.
    std::atomic<bool> cancel{true};
    ConsolidateHooks stop;
    stop.cancel = &cancel;
    const std::uint64_t rev = r.doc->current().revision;
    CHECK(!consolidate(*r.doc, *r.session, r.cfg.dir, rq, stop, updates, err));
    CHECK(err.find("cancelled") != std::string::npos);
    CHECK(updates.empty() && r.doc->current().revision == rev);
    CHECK(!fs::exists(r.cfg.dir + "/Audio Files/Audio 1_cons_02.wav"));

    CHECK(consolidate(*r.doc, *r.session, r.cfg.dir, rq, {}, updates, err));
    CHECK(fs::file_size(taken) == 4);   // untouched
    CHECK(fs::exists(r.cfg.dir + "/Audio Files/Audio 1_cons_02.wav"));
    const model::Playlist* p = playlist_of(r.doc->current(), r.playlist);
    CHECK(p->regions.size() == 1 && p->regions[0].length == 400);

    // A range holding no region is refused by the model before any render.
    ConsolidateRequest none = rq;
    none.start = 800;
    none.end = 900;
    CHECK(!consolidate(*r.doc, *r.session, r.cfg.dir, none, {}, updates, err));
    CHECK(!fs::exists(r.cfg.dir + "/Audio Files/Audio 1_cons_03.wav"));
    return 0;
}

int test_refused_while_recording() {
    Rig r;
    CHECK(make_rig(r, "test_consolidate_rec"));
    for (auto& t : r.session->tracks()) t->set_rec_armed(true);
    r.session->disk().start();
    CHECK(r.session->start_recording(1));
    ConsolidateRequest rq;
    rq.track = r.track;
    rq.start = 0;
    rq.end = 400;
    std::vector<model::ReplicationUpdate> updates;
    std::string err;
    const std::uint64_t rev = r.doc->current().revision;
    CHECK(!consolidate(*r.doc, *r.session, r.cfg.dir, rq, {}, updates, err));
    CHECK(err.find("recording") != std::string::npos);
    CHECK(updates.empty() && r.doc->current().revision == rev);
    CHECK(!fs::exists(r.cfg.dir + "/Audio Files/Audio 1_cons_01.wav"));
    CHECK(r.session->stop());
    return 0;
}

}  // namespace

int main() {
    if (int rc = test_two_takes_with_a_crossfade()) return rc;
    if (int rc = test_never_overwrites_and_cancel()) return rc;
    if (int rc = test_refused_while_recording()) return rc;
    std::printf("consolidate: ok\n");
    return 0;
}
