// SPDX-License-Identifier: GPL-3.0-or-later
//
// The null test of Export Stems (ADR-0085): the stems of a session, by track
// and by output, summed sample by sample, are the full mix bounce within one
// LSB at 24 bit. Four mono tracks, each panned its own way, two of them into
// an aux bus with its own gain and balance, and a send from a third into it,
// so pan, routing, the bus return and a send's share are all in the sum.
//
// Compared as float files with the int24 LSB as the bound: quantising each
// stem rounds it on its own, so n int24 stems sum to the int24 mix only
// within n half-LSBs, which says nothing about the render.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "sonora/bounce.h"
#include "sonora/closed_take.h"
#include "sonora/model/operations.h"
#include "sonora/model/replication.h"
#include "sonora/session.h"
#include "sonora/stems.h"
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

#define CHECK_OK(call, err)                                              \
    do {                                                                 \
        if (!(call)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         (err).c_str());                                 \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

using namespace sonora;
namespace fs = std::filesystem;

constexpr uint32_t kRate = 48000;
constexpr int64_t kLength = 48000;
constexpr int kTracks = 4;
// One LSB of a 24-bit file at full scale.
constexpr double kLsb24 = 1.0 / 8388608.0;

float tone(int track, int64_t i) {
    static const int periods[kTracks] = {48, 60, 80, 96};
    const int p = periods[track];
    return 0.1f * std::sin(2.0f * 3.14159265f * static_cast<float>(i % p) / static_cast<float>(p));
}

struct Rig {
    std::string dir;
    std::unique_ptr<Session> session;
    std::unique_ptr<model::Document> doc;
};

bool apply(Rig& r, const model::Operation& op, std::string& err) {
    model::ReplicationUpdate u;
    return r.doc->apply(op, u, err);
}

bool make_rig(Rig& r, const std::string& dir, std::string& err) {
    fs::remove_all(dir);
    r.dir = dir;
    SessionConfig cfg;
    cfg.dir = dir;
    cfg.name = "Null";
    cfg.track_count = kTracks;
    cfg.ring_seconds = 0.1;
    cfg.sample_rate = kRate;
    r.session = std::make_unique<Session>(cfg);
    if (!r.session->create_dirs()) return false;
    r.doc = std::make_unique<model::Document>(engine_document(*r.session));
    if (r.doc->current().edit->tracks.size() != kTracks) {
        err = "the engine document has the wrong track count";
        return false;
    }
    static const float pans[kTracks] = {-0.6f, 0.4f, -0.2f, 0.8f};
    for (int t = 0; t < kTracks; ++t) {
        const std::string rel = "Audio Files/t" + std::to_string(t) + ".wav";
        WavWriter w;
        if (!w.open(dir + "/" + rel, kRate, 1, WavWriter::Format::Float32, 0)) return false;
        std::vector<float> data(static_cast<std::size_t>(kLength));
        for (int64_t i = 0; i < kLength; ++i) data[static_cast<std::size_t>(i)] = tone(t, i);
        if (!w.write(data.data(), data.size()) || !w.close()) return false;

        const model::TrackEdit& track = *r.doc->current().edit->tracks[static_cast<std::size_t>(t)];
        model::AddSource add;
        add.source.id = r.doc->current().next_id;
        add.source.path = rel;
        add.source.channels = 1;
        add.source.length = kLength;
        add.source.format = model::AudioFormat::Float32;
        if (!apply(r, add, err)) return false;
        model::PlaceRegion p;
        p.playlist = track.active_playlist;
        p.region.id = r.doc->current().next_id;
        p.region.name = "r";
        p.region.position = 0;
        p.region.length = kLength;
        p.region.channels.push_back({add.source.id, 0});
        p.split_right_id = p.region.id + 1;
        if (!apply(r, p, err)) return false;

        const model::TrackMix* now = r.doc->current().mix.find(track.id);
        model::SetMix m;
        if (now != nullptr) m.mix = *now;
        m.mix.track = track.id;
        m.mix.pan = pans[t];
        m.mix.gain_db = -1.5f * static_cast<float>(t);
        if (!apply(r, m, err)) return false;
    }

    model::AddBus bus;
    bus.bus = r.doc->current().next_id;
    bus.name = "Bus";
    if (!apply(r, bus, err)) return false;
    model::SetBusMix bm;
    bm.mix.bus = bus.bus;
    bm.mix.gain_db = -4.5f;
    bm.mix.pan = 0.25f;
    if (!apply(r, bm, err)) return false;
    for (int t = 2; t < kTracks; ++t) {
        model::SetOutput o;
        o.strip = r.doc->current().edit->tracks[static_cast<std::size_t>(t)]->id;
        o.output = bus.bus;
        if (!apply(r, o, err)) return false;
    }
    model::AddSend send;
    send.strip = r.doc->current().edit->tracks[0]->id;
    send.send = r.doc->current().next_id;
    send.slot = 0;
    send.bus = bus.bus;
    send.mix.level_db = -6.0f;
    send.mix.follow_pan = true;
    return apply(r, send, err);
}

bool read_stereo(const std::string& path, std::vector<float>& out) {
    WavReader rd;
    std::string e;
    if (!rd.open(path, e) || rd.channels() != 2 || rd.frames() != kLength) return false;
    out.assign(static_cast<std::size_t>(kLength) * 2, 0.0f);
    return rd.read(0, kLength, out.data()) == kLength;
}

// The largest |sum of the stems - the mix| over every sample, or -1 when a
// file cannot be read.
double worst_difference(const std::string& dir, const std::vector<float>& mix,
                        const StemsResult& stems) {
    std::vector<double> sum(mix.size(), 0.0);
    for (const std::string& f : stems.files) {
        std::vector<float> d;
        if (!read_stereo(dir + "/" + f, d)) return -1.0;
        for (std::size_t i = 0; i < d.size(); ++i) sum[i] += d[i];
    }
    double worst = 0.0;
    for (std::size_t i = 0; i < mix.size(); ++i) {
        worst = std::max(worst, std::fabs(sum[i] - static_cast<double>(mix[i])));
    }
    return worst;
}

double peak(const std::vector<float>& d) {
    double p = 0.0;
    for (const float v : d) p = std::max(p, static_cast<double>(std::fabs(v)));
    return p;
}

int the_stems_sum_to_the_mix() {
    Rig r;
    std::string err;
    CHECK_OK(make_rig(r, "stems_sum_t1", err), err);
    const model::Snapshot snap = r.doc->current();

    api::BounceOptions full;
    full.file = api::BounceFile::Wav;
    full.format = api::BounceFormat::Float32;
    full.name = "Full";
    BounceHooks hooks;
    BounceResult mixed;
    CHECK_OK(bounce_offline(snap, r.dir, full, hooks, mixed, err), err);
    std::vector<float> mix;
    CHECK(read_stereo(r.dir + "/" + mixed.relative_path, mix));
    // Not a null test of silence.
    CHECK(peak(mix) > 0.05);

    const struct {
        const char* arg;
        std::size_t files;
    } cases[] = {{"by=track file=wav format=float32 name=Tracks", 4},
                 {"by=output file=wav format=float32 name=Outputs", 2}};
    for (const auto& c : cases) {
        api::StemsOptions o;
        CHECK_OK(api::parse_stems_argument(c.arg, o, err), err);
        StemsResult stems;
        CHECK_OK(stems_offline(snap, r.dir, o, hooks, stems, err), err);
        CHECK(stems.files.size() == c.files);
        CHECK(stems.frames == static_cast<uint64_t>(kLength));
        const double worst = worst_difference(r.dir, mix, stems);
        std::printf("%s: %zu stems, worst |sum - mix| = %.3g (1 LSB at 24 bit = %.3g)\n", c.arg,
                    stems.files.size(), worst, kLsb24);
        CHECK(worst >= 0.0 && worst <= kLsb24);
        // No stem is silent, so none hides another's part.
        for (const std::string& f : stems.files) {
            std::vector<float> d;
            CHECK(read_stereo(r.dir + "/" + f, d));
            CHECK(peak(d) > 0.005);
        }
    }
    // The document was never touched: no solo, the mix as it was.
    CHECK(r.doc->current() == snap);
    return 0;
}

// The same null test with every file int24, undithered: what a user gets.
// Each stem and the mix round on their own, so the sum of n stems is off the
// mix by up to (n + 1) half-LSBs; that is the bound, and a real error in the
// render (a pan, a bus gain) is thousands of LSBs, far above it.
int the_int24_stems_sum_to_the_int24_mix() {
    Rig r;
    std::string err;
    CHECK_OK(make_rig(r, "stems_sum_t2", err), err);
    const model::Snapshot snap = r.doc->current();

    api::BounceOptions full;
    CHECK_OK(api::parse_bounce_argument("file=wav format=int24 dither=none name=Full24", full, err),
             err);
    CHECK(full.format == api::BounceFormat::Int24);
    BounceHooks hooks;
    BounceResult mixed;
    CHECK_OK(bounce_offline(snap, r.dir, full, hooks, mixed, err), err);
    std::vector<float> mix;
    CHECK(read_stereo(r.dir + "/" + mixed.relative_path, mix));
    CHECK(peak(mix) > 0.05);

    api::StemsOptions o;
    CHECK_OK(api::parse_stems_argument("by=track file=wav format=int24 dither=none name=Tracks24", o,
                                       err),
             err);
    CHECK(o.base.format == api::BounceFormat::Int24);
    StemsResult stems;
    CHECK_OK(stems_offline(snap, r.dir, o, hooks, stems, err), err);
    CHECK(stems.files.size() == static_cast<std::size_t>(kTracks));
    const double worst = worst_difference(r.dir, mix, stems);
    const double bound = 0.5 * (kTracks + 1) * kLsb24 * 1.001;
    std::printf("int24: %zu stems, worst |sum - mix| = %.3g (%.2f LSB; bound %.2f LSB)\n",
                stems.files.size(), worst, worst / kLsb24, bound / kLsb24);
    CHECK(worst >= 0.0 && worst <= bound);
    return 0;
}

}  // namespace

int main() {
    int failures = the_stems_sum_to_the_mix();
    if (failures == 0) failures = the_int24_stems_sum_to_the_int24_mix();
    if (failures == 0) std::puts("stems_sum: all passed");
    return failures == 0 ? 0 : 1;
}
