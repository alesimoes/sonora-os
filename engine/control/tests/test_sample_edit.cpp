// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sample-level editing (ADR-0052): each process by hand, the request parser,
// and the whole edit: the take is never touched, the copy holds the edited
// stretch and a handle, the region points at it with its gain line moved, one
// undo step puts the original back, and a refusal leaves no file behind.
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "sonora/closed_take.h"
#include "sonora/model/operations.h"
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

bool near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

// The sample at frame i of the test source, the same on every channel.
float source_at(std::int64_t i) { return static_cast<float>((i * 7) % 1000) / 1000.0f - 0.5f; }

SampleEditRequest request(SampleVerb verb, model::Id region, model::Frames from, model::Frames to) {
    SampleEditRequest r;
    r.verb = verb;
    r.region = region;
    r.from = from;
    r.to = to;
    return r;
}

int test_processes() {
    std::string why;
    // Repair: a straight ramp with a click redrawn comes back a straight ramp.
    {
        std::vector<float> buf(40);
        for (std::size_t i = 0; i < buf.size(); ++i) buf[i] = 0.01f * static_cast<float>(i);
        const std::vector<float> clean = buf;
        for (std::size_t i = 10; i < 14; ++i) buf[i] = 0.9f;
        CHECK(process_samples(request(SampleVerb::Repair, 1, 0, 4), buf, 10, 4, 26, 10, why));
        for (std::size_t i = 0; i < buf.size(); ++i) CHECK(near(buf[i], clean[i], 1e-5f));
        // Without context before it holds the sample after.
        std::vector<float> edge{0.9f, 0.9f, 0.2f, 0.2f};
        CHECK(process_samples(request(SampleVerb::Repair, 1, 0, 2), edge, 0, 2, 2, 0, why));
        CHECK(near(edge[0], 0.2f) && near(edge[1], 0.2f) && near(edge[2], 0.2f));
        // A click is a few samples, not a passage.
        std::vector<float> big(kMaxRepairFrames + 10, 0.5f);
        CHECK(!process_samples(request(SampleVerb::Repair, 1, 0, kMaxRepairFrames + 1), big, 4,
                               static_cast<std::size_t>(kMaxRepairFrames + 1), 5, 0, why));
        CHECK(why.find("at most") != std::string::npos);
    }
    // Draw: points joined by lines, held outside them, in timeline frames.
    {
        std::vector<float> buf(6, 0.7f);
        SampleEditRequest r = request(SampleVerb::Draw, 1, 100, 106);
        r.points = {{101, 0.0f}, {104, 0.6f}};
        CHECK(process_samples(r, buf, 0, 6, 0, 100, why));
        const float want[6] = {0.0f, 0.0f, 0.2f, 0.4f, 0.6f, 0.6f};
        for (int i = 0; i < 6; ++i) CHECK(near(buf[static_cast<std::size_t>(i)], want[i]));
        r.points.clear();
        CHECK(!process_samples(r, buf, 0, 6, 0, 100, why));
    }
    // The simple ones.
    {
        const std::vector<float> in{0.1f, -0.2f, 0.4f, 0.8f};
        auto run = [&](SampleEditRequest r, std::vector<float>& out) {
            out = in;
            return process_samples(r, out, 0, out.size(), 0, 0, why);
        };
        std::vector<float> out;
        CHECK(run(request(SampleVerb::Silence, 1, 0, 4), out) && out == std::vector<float>(4, 0.0f));
        CHECK(run(request(SampleVerb::Reverse, 1, 0, 4), out) &&
              out == std::vector<float>({0.8f, 0.4f, -0.2f, 0.1f}));
        CHECK(run(request(SampleVerb::Invert, 1, 0, 4), out) &&
              out == std::vector<float>({-0.1f, 0.2f, -0.4f, -0.8f}));
        SampleEditRequest g = request(SampleVerb::Gain, 1, 0, 4);
        g.db = 6.0206f;
        CHECK(run(g, out) && near(out[3], 1.6f, 1e-3f));
        SampleEditRequest n = request(SampleVerb::Normalize, 1, 0, 4);
        n.db = -6.0206f;
        CHECK(run(n, out) && near(out[3], 0.5f, 1e-3f) && near(out[1], -0.125f, 1e-3f));
        CHECK(run(request(SampleVerb::FadeIn, 1, 0, 4), out) && out[0] == 0.0f &&
              near(out[3], 0.8f * 0.75f));
        CHECK(run(request(SampleVerb::FadeOut, 1, 0, 4), out) && out[3] == 0.0f &&
              near(out[0], 0.1f * 0.75f));
        std::vector<float> silent(4, 0.0f);
        CHECK(!process_samples(n, silent, 0, 4, 0, 0, why));
        CHECK(why.find("silent") != std::string::npos);
    }
    return 0;
}

int test_parse() {
    SampleEditRequest r;
    std::string why;
    CHECK(parse_sample_edit("repair 12 100 164 all", r, why));
    CHECK(r.verb == SampleVerb::Repair && r.region == 12 && r.from == 100 && r.to == 164 &&
          r.channel == -1);
    CHECK(parse_sample_edit("gain 3 0 10 2 -3.5", r, why));
    CHECK(r.verb == SampleVerb::Gain && r.channel == 1 && near(r.db, -3.5f));
    CHECK(parse_sample_edit("normalize 3 0 10 all", r, why) && r.db == 0.0f);
    CHECK(parse_sample_edit("draw 3 0 10 all 1:0.5,4:-0.5", r, why));
    CHECK(r.points.size() == 2 && r.points[1].first == 4 && near(r.points[1].second, -0.5f));
    CHECK(!parse_sample_edit("", r, why));
    CHECK(!parse_sample_edit("smudge 3 0 10 all", r, why) && why.find("smudge") != std::string::npos);
    CHECK(!parse_sample_edit("repair 3 10 10 all", r, why));
    CHECK(!parse_sample_edit("repair 3 0 10 0", r, why) && why.find("channel") != std::string::npos);
    CHECK(!parse_sample_edit("gain 3 0 10 all", r, why) && why.find("dB") != std::string::npos);
    CHECK(!parse_sample_edit("gain 3 0 10 all 99", r, why));
    CHECK(!parse_sample_edit("draw 3 0 10 all 4:0.5,1:0", r, why) &&
          why.find("increasing") != std::string::npos);
    CHECK(!parse_sample_edit("draw 3 0 10 all 4:2", r, why));
    CHECK(!parse_sample_edit("silence 3 0 10 all 1", r, why));
    CHECK(!parse_sample_edit("silence 3 0 10 all junk more", r, why));
    return 0;
}

struct Rig {
    SessionConfig cfg;
    std::unique_ptr<Session> session;
    std::unique_ptr<model::Document> doc;
    model::Id track = 0, playlist = 0, source = 0, region = 0;
    std::uint16_t channels = 0;
    std::uint32_t rate = 0;
};

bool write_source(const std::string& path, std::uint32_t rate, std::uint16_t channels,
                  std::int64_t frames) {
    WavWriter w;
    if (!w.open(path, rate, channels, WavWriter::Format::Float32, 0)) return false;
    std::vector<float> block(static_cast<std::size_t>(frames) * channels);
    for (std::int64_t i = 0; i < frames; ++i) {
        for (std::uint16_t c = 0; c < channels; ++c) {
            block[static_cast<std::size_t>(i) * channels + c] = source_at(i);
        }
    }
    return w.write(block.data(), static_cast<std::size_t>(frames)) && w.close();
}

// A source of `frames` frames; one region of `length` frames at `position`,
// playing from source frame `offset`, with `line` as its gain line.
bool make_rig(Rig& r, const char* dir, std::int64_t frames, model::Frames position,
              model::Frames length, model::Frames offset,
              std::vector<model::GainPoint> line = {}) {
    fs::remove_all(dir);
    r.cfg.dir = dir;
    r.cfg.track_count = 1;
    r.cfg.ring_seconds = 0.01;
    r.cfg.format = WavWriter::Format::Float32;
    r.session = std::make_unique<Session>(r.cfg);
    if (!r.session->create_dirs()) return false;
    r.doc = std::make_unique<model::Document>(engine_document(*r.session));
    const model::TrackEdit& t = *r.doc->current().edit->tracks[0];
    r.track = t.id;
    r.playlist = t.active_playlist;
    r.channels = t.channels;
    r.rate = r.doc->current().settings.sample_rate;
    if (!write_source(r.cfg.dir + "/Audio Files/take.wav", r.rate, r.channels, frames)) return false;
    std::string err;
    model::ReplicationUpdate u;
    model::AddSource add;
    add.source.id = r.doc->current().next_id;
    add.source.path = "Audio Files/take.wav";
    add.source.channels = r.channels;
    add.source.length = frames;
    add.source.format = model::AudioFormat::Float32;
    r.source = add.source.id;
    if (!r.doc->apply(add, u, err)) return false;
    model::PlaceRegion p;
    p.playlist = r.playlist;
    p.region.id = r.doc->current().next_id;
    p.region.name = "vox";
    p.region.position = position;
    p.region.length = length;
    p.region.source_offset = offset;
    p.region.gain_line = std::move(line);
    for (std::uint16_t c = 0; c < r.channels; ++c) p.region.channels.push_back({r.source, c});
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

// Frame `i` of channel `c` of a file.
bool read_frame(const std::string& path, std::int64_t i, std::uint16_t c, float& out) {
    WavReader rd;
    std::string e;
    if (!rd.open(path, e)) return false;
    std::vector<float> v(rd.channels());
    if (rd.read(i, 1, v.data()) != 1) return false;
    out = v[c];
    return true;
}

int test_small_source() {
    Rig r;
    CHECK(make_rig(r, "test_sample_edit_small", 1000, 100, 300, 200));
    const std::string take = r.cfg.dir + "/Audio Files/take.wav";
    const std::string before = slurp(take);
    const std::size_t files = files_in(r.cfg.dir + "/Audio Files");
    const std::size_t undo = r.doc->history().undo_depth();
    std::vector<model::ReplicationUpdate> updates;
    std::string err;

    // Refusals leave the document and the folder as they were.
    CHECK(!sample_edit(*r.doc, *r.session, r.cfg.dir, request(SampleVerb::Silence, r.region, 50, 150),
                       updates, err));
    CHECK(err.find("inside the region") != std::string::npos);
    CHECK(!sample_edit(*r.doc, *r.session, r.cfg.dir, request(SampleVerb::Silence, 9999, 150, 160),
                       updates, err));
    SampleEditRequest onech = request(SampleVerb::Silence, r.region, 150, 160);
    onech.channel = 7;
    CHECK(!sample_edit(*r.doc, *r.session, r.cfg.dir, onech, updates, err));
    CHECK(updates.empty() && files_in(r.cfg.dir + "/Audio Files") == files &&
          r.doc->history().undo_depth() == undo);
    CHECK(sample_edit(*r.doc, *r.session, r.cfg.dir, request(SampleVerb::Silence, r.region, 150, 170),
                      updates, err));
    CHECK(updates.size() == 1);
    CHECK(r.doc->history().undo_depth() == undo + 1);
    CHECK(slurp(take) == before);
    CHECK(files_in(r.cfg.dir + "/Audio Files") == files + 1);

    const model::Region* g = region_of(r);
    CHECK(g != nullptr && g->position == 100 && g->length == 300 && g->source_offset == 200);
    CHECK(g->channels.size() == r.channels && g->channels[0].source != r.source);
    const model::Source* made = r.doc->current().sources->find(g->channels[0].source);
    CHECK(made != nullptr && made->path == "Audio Files/Audio 1_edit_01.wav" && made->length == 1000 &&
          made->channels == r.channels);
    const std::string copy = r.cfg.dir + "/" + made->path;
    // Source frames 250..269 are the stretch [150, 170): silent. The rest is the take's.
    for (std::int64_t i = 0; i < 1000; ++i) {
        float v = 9.0f;
        CHECK(read_frame(copy, i, 0, v));
        const float want = (i >= 250 && i < 270) ? 0.0f : source_at(i);
        if (!near(v, want)) {
            std::fprintf(stderr, "frame %lld: %f want %f\n", static_cast<long long>(i),
                         static_cast<double>(v), static_cast<double>(want));
            return 1;
        }
    }

    // One undo puts the region back on the original take.
    model::ReplicationUpdate u;
    CHECK(r.doc->undo(u, err));
    g = region_of(r);
    CHECK(g != nullptr && g->channels[0].source == r.source && g->source_offset == 200);
    CHECK(slurp(take) == before);

    // A repair of a click: a second edit numbers its file after the first.
    updates.clear();
    CHECK(sample_edit(*r.doc, *r.session, r.cfg.dir, request(SampleVerb::Repair, r.region, 150, 154),
                      updates, err));
    g = region_of(r);
    made = r.doc->current().sources->find(g->channels[0].source);
    CHECK(made != nullptr && made->path == "Audio Files/Audio 1_edit_02.wav");
    return 0;
}

int test_large_source_moves_the_gain_line() {
    Rig r;
    // 150000 frames in, so the 2 s handle (96000 at 48 kHz) starts the copy at 54000.
    CHECK(make_rig(r, "test_sample_edit_large", 200000, 100, 1000, 150000,
                   {{149000, -3.0f}, {150500, -6.0f}}));
    if (r.rate != 48000) return 0;
    std::vector<model::ReplicationUpdate> updates;
    std::string err;
    CHECK(sample_edit(*r.doc, *r.session, r.cfg.dir, request(SampleVerb::Invert, r.region, 110, 120),
                      updates, err));
    const model::Region* g = region_of(r);
    CHECK(g != nullptr);
    const model::Source* made = r.doc->current().sources->find(g->channels[0].source);
    CHECK(made != nullptr && made->length == 146000);
    CHECK(g->source_offset == 96000);
    CHECK(g->gain_line.size() == 2 && g->gain_line[0].at == 95000 && g->gain_line[0].db == -3.0f &&
          g->gain_line[1].at == 96500 && g->gain_line[1].db == -6.0f);
    const std::string copy = r.cfg.dir + "/" + made->path;
    float v = 0.0f;
    // Window frame w is source frame 54000 + w; the stretch is w 96010..96019.
    CHECK(read_frame(copy, 96009, 0, v) && near(v, source_at(54000 + 96009)));
    CHECK(read_frame(copy, 96010, 0, v) && near(v, -source_at(54000 + 96010)));
    CHECK(read_frame(copy, 96019, 0, v) && near(v, -source_at(54000 + 96019)));
    CHECK(read_frame(copy, 96020, 0, v) && near(v, source_at(54000 + 96020)));
    CHECK(read_frame(copy, 0, 0, v) && near(v, source_at(54000)));
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_processes();
    failed += test_parse();
    failed += test_small_source();
    failed += test_large_source_moves_the_gain_line();
    if (failed == 0) std::printf("ok\n");
    return failed == 0 ? 0 : 1;
}
