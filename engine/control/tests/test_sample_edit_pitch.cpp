// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `pitch` verb of sample.edit (ADR-0143), with the fake shifter: the
// parser, one undo step that leaves the take byte-identical, the refusals
// (range, overlap, take open, disk full, shifter failure, wrong length) that
// leave neither a file nor a history entry, and the capability without
// Rubber Band. Built from engine/pitch/CMakeLists.txt.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "sonora/closed_take.h"
#include "sonora/model/operations.h"
#include "sonora/pitch_render.h"
#include "sonora/sample_edit.h"
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

float source_at(std::int64_t i) { return static_cast<float>((i * 7) % 1000) / 1000.0f - 0.5f; }

struct Rig {
    SessionConfig cfg;
    std::unique_ptr<Session> session;
    std::unique_ptr<model::Document> doc;
    model::Id source = 0, region = 0;
    std::string take, audio;
};

// A 1000-frame take; one region of 300 frames at timeline 100 playing from
// source frame 200.
bool make_rig(Rig& r, const char* dir) {
    fs::remove_all(dir);
    r.cfg.dir = dir;
    r.cfg.track_count = 1;
    r.cfg.ring_seconds = 0.01;
    r.cfg.format = WavWriter::Format::Float32;
    r.session = std::make_unique<Session>(r.cfg);
    if (!r.session->create_dirs()) return false;
    r.doc = std::make_unique<model::Document>(engine_document(*r.session));
    const model::TrackEdit& t = *r.doc->current().edit->tracks[0];
    r.audio = r.cfg.dir + "/Audio Files";
    r.take = r.audio + "/take.wav";
    {
        WavWriter w;
        if (!w.open(r.take, r.doc->current().settings.sample_rate, t.channels,
                    WavWriter::Format::Float32, 0)) {
            return false;
        }
        std::vector<float> block(1000u * t.channels);
        for (std::size_t i = 0; i < 1000; ++i) {
            for (std::uint16_t c = 0; c < t.channels; ++c) block[i * t.channels + c] = source_at(i);
        }
        if (!w.write(block.data(), 1000) || !w.close()) return false;
    }
    std::string err;
    model::ReplicationUpdate u;
    model::AddSource add;
    add.source.id = r.doc->current().next_id;
    add.source.path = "Audio Files/take.wav";
    add.source.channels = t.channels;
    add.source.length = 1000;
    add.source.format = model::AudioFormat::Float32;
    r.source = add.source.id;
    if (!r.doc->apply(add, u, err)) return false;
    model::PlaceRegion p;
    p.playlist = t.active_playlist;
    p.region.id = r.doc->current().next_id;
    p.region.name = "vox";
    p.region.position = 100;
    p.region.length = 300;
    p.region.source_offset = 200;
    for (std::uint16_t c = 0; c < t.channels; ++c) p.region.channels.push_back({r.source, c});
    r.region = p.region.id;
    return r.doc->apply(p, u, err);
}

const model::Region* region_of(const Rig& r) {
    for (const auto& t : r.doc->current().edit->tracks) {
        for (const auto& p : t->playlists) {
            for (const model::Region& g : p->regions) {
                if (g.id == r.region) return &g;
            }
        }
    }
    return nullptr;
}

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

std::size_t files_in(const std::string& dir) {
    std::size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        (void)e;
        ++n;
    }
    return n;
}

api::PitchNote note(std::uint64_t start, std::uint64_t end, std::int32_t cents, double gain_db = 0.0) {
    api::PitchNote n;
    n.start = start;
    n.end = end;
    n.cents = cents;
    n.gain_db = gain_db;
    return n;
}

PitchRenderRequest request(const Rig& r, std::vector<api::PitchNote> notes,
                           model::Frames from = 100, model::Frames to = 400) {
    PitchRenderRequest q;
    q.region = r.region;
    q.from = from;
    q.to = to;
    q.spec.notes = std::move(notes);
    return q;
}

std::uint64_t plenty(const std::string&) { return std::numeric_limits<std::uint64_t>::max(); }

// Returns a result one frame short: the engine must refuse it, never ripple.
class ShortShifter : public pitch::PitchShifter {
public:
    bool shift(const pitch::Planar& input, uint32_t, const api::PitchEditSpec&,
               pitch::Planar& output, std::string&) override {
        output = input;
        for (auto& ch : output) ch.pop_back();
        return true;
    }
};

int test_parse() {
    SampleEditRequest r;
    std::string why;
    CHECK(parse_sample_edit("pitch 12 100 400 all n=10:50:300:0:-6;n=60:90:-150", r, why));
    CHECK(r.verb == SampleVerb::Pitch && r.region == 12 && r.from == 100 && r.to == 400 &&
          r.channel == -1);
    CHECK(r.pitch.notes.size() == 2 && r.pitch.notes[0].cents == 300 &&
          r.pitch.notes[0].gain_db == -6.0 && r.pitch.notes[1].start == 60 &&
          r.pitch.notes[1].cents == -150);
    CHECK(!parse_sample_edit("pitch 12 100 400 1 n=10:50:300", r, why));
    CHECK(why.find("every channel") != std::string::npos);
    CHECK(!parse_sample_edit("pitch 12 100 400 all", r, why));
    CHECK(!parse_sample_edit("pitch 12 100 400 all n=10:50:300;n=40:60:0", r, why));
    CHECK(!parse_sample_edit("pitch 12 100 400 all n=10:50:9000", r, why));
    // The other verbs are as they were.
    CHECK(parse_sample_edit("silence 12 100 164 all", r, why) && r.verb == SampleVerb::Silence);
    std::vector<float> buf(4, 0.5f);
    SampleEditRequest p;
    p.verb = SampleVerb::Pitch;
    CHECK(!process_samples(p, buf, 0, 4, 0, 0, why));
    return 0;
}

int test_one_undo_step_and_the_take_untouched() {
    Rig r;
    CHECK(make_rig(r, "test_pitch_apply"));
    const std::string before = slurp(r.take);
    const std::size_t files = files_in(r.audio);
    const std::size_t undo = r.doc->history().undo_depth();
    pitch::FakePitchShifter fake;
    std::vector<model::ReplicationUpdate> updates;
    std::string err;
    // Region frames 10..50 at -6 dB, i.e. source frames 210..250.
    CHECK(render_pitch(*r.doc, *r.session, r.cfg.dir, request(r, {note(10, 50, 300, -6.0)}), fake,
                       plenty, updates, err));
    CHECK(updates.size() == 1);
    CHECK(r.doc->history().undo_depth() == undo + 1);
    CHECK(slurp(r.take) == before);
    CHECK(files_in(r.audio) == files + 1);
    // The shifter saw the whole copy, the notes moved by the lead handle.
    CHECK(fake.calls().size() == 1 && fake.calls()[0].frames == 1000 &&
          fake.calls()[0].spec.notes[0].start == 210 && fake.calls()[0].spec.notes[0].end == 250);

    const model::Region* g = region_of(r);
    CHECK(g != nullptr && g->position == 100 && g->length == 300 && g->source_offset == 200);
    CHECK(g->channels[0].source != r.source);
    const model::Source* made = r.doc->current().sources->find(g->channels[0].source);
    CHECK(made != nullptr && made->path == "Audio Files/Audio 1_pitch_01.wav" && made->length == 1000);
    WavReader rd;
    std::string why;
    CHECK(rd.open(r.cfg.dir + "/" + made->path, why));
    std::vector<float> all(static_cast<std::size_t>(1000) * rd.channels());
    CHECK(rd.read(0, 1000, all.data()) == 1000);
    const float g6 = static_cast<float>(std::pow(10.0, -6.0 / 20.0));
    for (std::int64_t i = 0; i < 1000; ++i) {
        const float want = (i >= 210 && i < 250) ? source_at(i) * g6 : source_at(i);
        CHECK(std::fabs(all[static_cast<std::size_t>(i) * rd.channels()] - want) < 1e-5f);
    }

    model::ReplicationUpdate u;
    CHECK(r.doc->undo(u, err));
    g = region_of(r);
    CHECK(g != nullptr && g->channels[0].source == r.source && g->source_offset == 200);
    CHECK(r.doc->history().undo_depth() == undo);
    CHECK(slurp(r.take) == before);
    return 0;
}

int test_refusals_leave_nothing() {
    Rig r;
    CHECK(make_rig(r, "test_pitch_refusals"));
    const std::string before = slurp(r.take);
    const std::size_t files = files_in(r.audio);
    const std::size_t undo = r.doc->history().undo_depth();
    pitch::FakePitchShifter fake;
    std::vector<model::ReplicationUpdate> updates;
    std::string err;
    const auto refused = [&](const PitchRenderRequest& q, pitch::PitchShifter& s, const FreeBytes& fb,
                             const char* why) {
        err.clear();
        const bool ok = render_pitch(*r.doc, *r.session, r.cfg.dir, q, s, fb, updates, err);
        if (ok || err.find(why) == std::string::npos) {
            std::fprintf(stderr, "expected \"%s\", got \"%s\"\n", why, err.c_str());
            return false;
        }
        return updates.empty() && files_in(r.audio) == files &&
               r.doc->history().undo_depth() == undo && slurp(r.take) == before;
    };
    // Past the region's 300 frames, outside the selection, outside the region.
    CHECK(refused(request(r, {note(250, 350, 100)}), fake, plenty, "ends past the region"));
    CHECK(refused(request(r, {note(0, 10, 100)}, 150, 200), fake, plenty, "outside the selection"));
    CHECK(refused(request(r, {note(0, 10, 100)}, 50, 200), fake, plenty, "inside the region"));
    // Overlapping notes built by hand, past the parser.
    CHECK(refused(request(r, {note(10, 50, 100), note(40, 60, 0)}), fake, plenty, "overlap"));
    CHECK(refused(request(r, {}), fake, plenty, "no notes"));
    PitchRenderRequest nowhere = request(r, {note(10, 50, 100)});
    nowhere.region = 9999;
    CHECK(refused(nowhere, fake, plenty, "no such region"));
    // A full disk costs no write.
    CHECK(refused(request(r, {note(10, 50, 100)}), fake,
                  [](const std::string&) { return std::uint64_t{4096}; }, "free space"));
    CHECK(fake.calls().empty());
    // A shifter that fails, or returns another length, after the file was
    // created: the partial file is removed.
    fake.refuse_with("pitch: the shifter gave up");
    CHECK(refused(request(r, {note(10, 50, 100)}), fake, plenty, "the shifter gave up"));
    ShortShifter short_one;
    CHECK(refused(request(r, {note(10, 50, 100)}), short_one, plenty, "length"));
    CHECK(!fs::exists(r.audio + "/Audio 1_pitch_01.wav"));
    return 0;
}

int test_take_open_refused() {
    Rig r;
    CHECK(make_rig(r, "test_pitch_take_open"));
    const std::string before = slurp(r.take);
    const std::size_t undo = r.doc->history().undo_depth();
    pitch::FakePitchShifter fake;
    std::vector<model::ReplicationUpdate> updates;
    std::string err;
    r.session->tracks()[0]->set_rec_armed(true);
    CHECK(r.session->start_recording(1, &err));
    CHECK(!render_pitch(*r.doc, *r.session, r.cfg.dir, request(r, {note(10, 50, 100)}), fake, plenty,
                        updates, err));
    CHECK(err.find("stop recording") != std::string::npos);
    CHECK(updates.empty() && fake.calls().empty() && r.doc->history().undo_depth() == undo);
    CHECK(!fs::exists(r.audio + "/Audio 1_pitch_01.wav"));
    CHECK(slurp(r.take) == before);
    return 0;
}

// Without Rubber Band the shifter is absent and the verb refuses, saying so;
// with it, sample.edit reaches the render.
int test_capability_follows_the_build() {
    CHECK(pitch_shifter_built() == (SONORA_PITCH_RUBBERBAND != 0));
    CHECK((make_pitch_shifter() != nullptr) == pitch_shifter_built());
    Rig r;
    CHECK(make_rig(r, "test_pitch_capability"));
    const std::size_t files = files_in(r.audio);
    SampleEditRequest q;
    std::string err;
    CHECK(parse_sample_edit("pitch " + std::to_string(r.region) + " 100 400 all n=10:50:0:0:-3", q, err));
    std::vector<model::ReplicationUpdate> updates;
    const bool ok = sample_edit(*r.doc, *r.session, r.cfg.dir, q, updates, err);
    if (pitch_shifter_built()) {
        CHECK(ok && updates.size() == 1 && files_in(r.audio) == files + 1);
    } else {
        CHECK(!ok && err.find("not in this build") != std::string::npos);
        CHECK(updates.empty() && files_in(r.audio) == files);
    }
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_parse();
    failed += test_one_undo_step_and_the_take_untouched();
    failed += test_refusals_leave_nothing();
    failed += test_take_open_refused();
    failed += test_capability_follows_the_build();
    if (failed == 0) std::printf("ok\n");
    return failed == 0 ? 0 : 1;
}
