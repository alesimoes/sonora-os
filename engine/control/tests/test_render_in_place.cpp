// SPDX-License-Identifier: GPL-3.0-or-later
//
// Render in place, the engine's half (ADR-0051): the file a freeze prints is
// the chain's output with its latency taken off, to the sample, so the
// printed track lands exactly where the live one played; a selection starts
// where it was asked, the lead-in discarded; a chain that was not bound, one
// that missed a block, a track edited or a bypass moved while it rendered
// each leave no file; the compound the print makes is one undo step; and a
// cancel removes the file. The chain is a test runner that delays by its
// declared latency, as engine/core's PDC tests do; no host is involved (the
// real runner is engine/plugin_client).
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "sonora/closed_take.h"
#include "sonora/insert_chain_runner.h"
#include "sonora/model/operations.h"
#include "sonora/model/render_in_place.h"
#include "sonora/playback_controller.h"
#include "sonora/render_in_place.h"
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

#define CHECK_OK(call, err)                                                  \
    do {                                                                     \
        if (!(call)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         (err).c_str());                                     \
            return 1;                                                        \
        }                                                                    \
    } while (0)

namespace {

using namespace sonora;
namespace fs = std::filesystem;

constexpr uint32_t kRate = 48000;
constexpr int64_t kLength = 48000;   // 1 s: it renders in real time
constexpr uint32_t kLatency = 300;
constexpr float kAmp = 0.1f;

float sine_at(int64_t i) {
    return kAmp * std::sin(2.0f * 3.14159265f * static_cast<float>(i % 48) / 48.0f);
}

// A chain that returns its input kLatency frames late, exactly: a host with
// that declared latency.
class DelayRunner final : public InsertChainRunner {
public:
    void begin(uint64_t) noexcept SONORA_RT override {}
    void end() noexcept SONORA_RT override {}
    uint32_t block_frames() const noexcept SONORA_RT override { return 0; }
    void skip(const PlanNode&, float* const out[2], uint32_t frames) noexcept SONORA_RT override {
        for (uint32_t i = 0; i < frames; ++i) out[0][i] = out[1][i] = 0.0f;
    }
    void run(const PlanNode& node, const float* const in[2], float* const out[2], uint32_t frames,
             const ChainParamEvent*, uint32_t) noexcept SONORA_RT override {
        for (uint32_t i = 0; i < frames; ++i) {
            ring_[pos_] = in[0][i];
            out[0][i] = ring_[(pos_ + kRing - node.latency_frames) % kRing];
            out[1][i] = out[0][i];
            pos_ = (pos_ + 1) % kRing;
        }
    }

private:
    static constexpr uint32_t kRing = 4096;
    float ring_[kRing] = {};
    uint32_t pos_ = 0;
};

// The audio thread: blocks of 256 frames, a little faster than real time.
struct Driver {
    explicit Driver(Session& s) : s_(s) {
        thread_ = std::thread([this] {
            std::vector<float> l(256), rr(256);
            float* outs[2] = {l.data(), rr.data()};
            uint64_t n = 0;
            while (run_.load()) {
                backend::AudioBlock b{};
                b.outputs = outs;
                b.output_count = 2;
                b.nframes = 256;
                b.frame_time = n++ * 256;
                b.callback_ns = 1'000'000'000 + n * 5'000'000;
                s_.process(b);
                std::this_thread::sleep_for(std::chrono::microseconds(1500));
            }
        });
    }
    ~Driver() {
        run_.store(false);
        thread_.join();
    }
    Session& s_;
    std::atomic<bool> run_{true};
    std::thread thread_;
};

struct Rig {
    std::string dir;
    std::unique_ptr<Session> session;
    std::unique_ptr<model::Document> doc;
    DelayRunner runner;
    bool bound = true;
    model::Id track = 0;
    model::Id insert = 0;
    model::Id playlist = 0;
    std::unique_ptr<PlaybackController> ctl;
};

// One mono track playing a 1 kHz sine from frame 0 for kLength frames, with
// one active insert whose chain is the delay runner (or, `bound` false, no
// host yet).
bool make_rig(Rig& r, const std::string& dir, bool bound = true) {
    fs::remove_all(dir);
    r.dir = dir;
    r.bound = bound;
    SessionConfig cfg;
    cfg.dir = dir;
    cfg.track_count = 1;
    cfg.ring_seconds = 0.1;
    cfg.sample_rate = kRate;
    r.session = std::make_unique<Session>(cfg);
    if (!r.session->create_dirs()) return false;
    r.doc = std::make_unique<model::Document>(engine_document(*r.session));
    WavWriter w;
    if (!w.open(dir + "/Audio Files/s.wav", kRate, 1, WavWriter::Format::Float32, 0)) return false;
    std::vector<float> data(static_cast<std::size_t>(kLength));
    for (int64_t i = 0; i < kLength; ++i) data[static_cast<std::size_t>(i)] = sine_at(i);
    if (!w.write(data.data(), data.size()) || !w.close()) return false;

    const model::TrackEdit& t = *r.doc->current().edit->tracks[0];
    r.track = t.id;
    r.playlist = t.active_playlist;
    std::string err;
    model::ReplicationUpdate u;
    model::AddSource add;
    add.source.id = r.doc->current().next_id;
    add.source.path = "Audio Files/s.wav";
    add.source.channels = 1;
    add.source.length = kLength;
    add.source.format = model::AudioFormat::Float32;
    if (!r.doc->apply(add, u, err)) return false;
    model::PlaceRegion p;
    p.playlist = t.active_playlist;
    p.region.id = r.doc->current().next_id;
    p.region.name = "r";
    p.region.position = 0;
    p.region.length = kLength;
    p.region.channels.push_back({add.source.id, 0});
    p.split_right_id = p.region.id + 100;
    if (!r.doc->apply(p, u, err)) return false;
    model::AddInsert in;
    in.track = r.track;
    in.insert = r.doc->current().next_id;
    in.slot = 0;
    in.plugin = {model::PluginFormat::Lv2, "urn:test:delay", "Delay", "1"};
    r.insert = in.insert;
    if (!r.doc->apply(in, u, err)) return false;

    r.session->reader().start();
    r.session->set_insert_chains(&r.runner);
    r.ctl = std::make_unique<PlaybackController>(*r.session);
    Rig* rig = &r;
    r.ctl->set_chain_resolver([rig](const model::TrackEdit&, uint32_t, ChainBinding& b) {
        if (!rig->bound) return;   // starting: dry, unbound
        b.channel_slot = 0;
        b.channel_generation = 0;
        b.param_store = 7;
        b.node_state = 0;
        b.latency_frames = kLatency;
    });
    if (!r.ctl->show(r.doc->current(), err)) return false;
    if (!r.ctl->settle(5.0)) return false;
    r.ctl->apply_mix(r.doc->current());
    return true;
}

// Plans a request, starts it, drives the audio thread and the control loop
// until it ends. Returns false and says why in `err` when it failed.
bool render(Rig& r, model::RenderKind kind, uint64_t sel_start, uint64_t sel_end,
            RenderInPlace::ChainMisses misses, RenderResult& res, RenderRequest& req,
            RenderBasis& basis, std::string& err, bool* cancelled = nullptr,
            bool cancel_soon = false) {
    RenderAsk ask;
    ask.track = r.track;
    ask.start = sel_start;
    ask.end = sel_end;
    ask.tail_ms = 0;
    ask.sample_rate = kRate;
    if (!plan_render(r.doc->current(), kind, ask, req, err)) return false;
    RenderInPlace rt(*r.session, *r.ctl);
    bool was_cancelled = false;
    {
        Driver audio(*r.session);
        if (!rt.start(r.doc->current(), r.dir, req, misses, err)) return false;
        std::string again;
        if (rt.start(r.doc->current(), r.dir, req, misses, again) || again.empty()) {
            err = "a second render started";
            return false;
        }
        if (cancel_soon) {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            rt.cancel();
        }
        bool ended = false;
        for (int i = 0; i < 30000 && !ended; ++i) {
            r.ctl->pump();
            ended = rt.poll(res, err, was_cancelled);
            if (!ended) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!ended) {
            err = "the render never ended";
            return false;
        }
    }
    basis = rt.basis();
    if (cancelled != nullptr) *cancelled = was_cancelled;
    return err.empty();
}

std::size_t audio_files(const Rig& r) {
    std::size_t n = 0;
    for (const auto& e : fs::directory_iterator(r.dir + "/Audio Files")) {
        (void)e;
        ++n;
    }
    return n;
}

int freeze_prints_the_chain_aligned_to_the_sample() {
    Rig r;
    CHECK(make_rig(r, "render_t1"));
    RenderResult res;
    RenderRequest req;
    RenderBasis basis;
    std::string err;
    CHECK_OK(render(r, model::RenderKind::Freeze, 0, 0, nullptr, res, req, basis, err), err);
    CHECK(res.frames == static_cast<uint64_t>(kLength));
    CHECK(res.start == 0);
    CHECK(r.session->transport().state() == TransportState::Stopped);

    // The chain delayed the audio by kLatency; the file has it taken off, so
    // it is the source itself where the live track would have played it.
    WavReader rd;
    CHECK(rd.open(r.dir + "/" + res.relative_path, err));
    CHECK(rd.channels() == 1 && rd.frames() == kLength);
    std::vector<float> d(static_cast<std::size_t>(kLength));
    CHECK(rd.read(0, kLength, d.data()) == kLength);
    for (int64_t i = 0; i < kLength; ++i) {
        if (d[static_cast<std::size_t>(i)] != sine_at(i)) {
            std::fprintf(stderr, "frame %lld: %g, wanted %g\n", static_cast<long long>(i),
                         static_cast<double>(d[static_cast<std::size_t>(i)]),
                         static_cast<double>(sine_at(i)));
            return 1;
        }
    }
    CHECK(rd.time_reference() == 0);
    rd.close();

    // Put in the document: one step, the track plays the print, the insert is
    // off; undo returns both.
    const model::Snapshot before = r.doc->current();
    std::vector<model::ReplicationUpdate> updates;
    CHECK_OK(place_render(*r.doc, r.dir, req, basis, res, updates, err), err);
    CHECK(updates.size() == 1);
    const model::TrackEdit& t = *r.doc->current().edit->tracks[0];
    CHECK(model::frozen_playlist(t) != nullptr);
    CHECK(!t.inserts[0].active);
    CHECK(t.active_playlist != r.playlist);

    CHECK_OK(unfreeze_track(*r.doc, r.track, updates, err), err);
    const model::TrackEdit& back = *r.doc->current().edit->tracks[0];
    CHECK(back.active_playlist == r.playlist && back.inserts[0].active);
    CHECK(model::frozen_playlist(back) == nullptr);
    CHECK(!unfreeze_track(*r.doc, r.track, updates, err) && err.find("not frozen") != std::string::npos);
    return 0;
}

int a_selection_starts_where_it_was_asked() {
    Rig r;
    CHECK(make_rig(r, "render_t2"));
    RenderResult res;
    RenderRequest req;
    RenderBasis basis;
    std::string err;
    // 12000 frames in: the lead-in before it is rolled and thrown away.
    CHECK_OK(render(r, model::RenderKind::Process, 12000, 30000, nullptr, res, req, basis, err),
             err);
    CHECK(res.frames == 18000 && res.start == 12000);
    WavReader rd;
    CHECK(rd.open(r.dir + "/" + res.relative_path, err));
    CHECK(rd.time_reference() == 12000);
    std::vector<float> d(18000);
    CHECK(rd.read(0, 18000, d.data()) == 18000);
    for (int64_t i = 0; i < 18000; ++i) {
        CHECK(d[static_cast<std::size_t>(i)] == sine_at(12000 + i));
    }
    rd.close();
    std::vector<model::ReplicationUpdate> updates;
    CHECK_OK(place_render(*r.doc, r.dir, req, basis, res, updates, err), err);
    const model::TrackEdit& t = *r.doc->current().edit->tracks[0];
    CHECK(t.playlists.size() == 2 && t.active_playlist != r.playlist);
    CHECK(model::frozen_playlist(t) == nullptr);
    return 0;
}

int an_unbound_chain_leaves_no_file() {
    Rig r;
    CHECK(make_rig(r, "render_t3", false));
    RenderResult res;
    RenderRequest req;
    RenderBasis basis;
    std::string err;
    const std::size_t files = audio_files(r);
    CHECK(!render(r, model::RenderKind::Freeze, 0, 0, nullptr, res, req, basis, err));
    CHECK(err.find("did not play") != std::string::npos);
    CHECK(audio_files(r) == files);
    CHECK(r.session->transport().state() == TransportState::Stopped);
    return 0;
}

int a_missed_block_leaves_no_file() {
    Rig r;
    CHECK(make_rig(r, "render_t4"));
    RenderResult res;
    RenderRequest req;
    RenderBasis basis;
    std::string err;
    uint64_t reading = 0;
    const std::size_t files = audio_files(r);
    CHECK(!render(r, model::RenderKind::Freeze, 0, 0, [&reading] { return reading++; }, res, req,
                  basis, err));
    CHECK(err.find("missed its deadline") != std::string::npos);
    CHECK(audio_files(r) == files);
    return 0;
}

int an_edit_while_it_rendered_is_refused() {
    Rig r;
    CHECK(make_rig(r, "render_t5"));
    RenderResult res;
    RenderRequest req;
    RenderBasis basis;
    std::string err;
    CHECK_OK(render(r, model::RenderKind::Freeze, 0, 0, nullptr, res, req, basis, err), err);
    const std::size_t files = audio_files(r);
    std::vector<model::ReplicationUpdate> updates;

    // The insert was turned off meanwhile: the print has a chain the track no
    // longer runs.
    model::ReplicationUpdate u;
    CHECK(r.doc->apply(model::SetInsertActive{r.insert, false}, u, err));
    CHECK(!place_render(*r.doc, r.dir, req, basis, res, updates, err));
    CHECK(err.find("edited while it rendered") != std::string::npos);
    CHECK(audio_files(r) == files - 1);   // the print is removed
    CHECK(updates.empty());
    return 0;
}

int a_bypass_while_it_rendered_is_refused() {
    Rig r;
    CHECK(make_rig(r, "render_t6"));
    RenderResult res;
    RenderRequest req;
    RenderBasis basis;
    std::string err;
    CHECK_OK(render(r, model::RenderKind::Commit, 0, 0, nullptr, res, req, basis, err), err);
    model::ReplicationUpdate u;
    CHECK(r.doc->apply(model::SetInsertBypass{r.insert, true}, u, err));
    std::vector<model::ReplicationUpdate> updates;
    CHECK(!place_render(*r.doc, r.dir, req, basis, res, updates, err));
    CHECK(err.find("edited while it rendered") != std::string::npos);
    CHECK(!fs::exists(r.dir + "/" + res.relative_path));
    return 0;
}

int a_cancel_removes_the_file_and_changes_nothing() {
    Rig r;
    CHECK(make_rig(r, "render_t7"));
    RenderResult res;
    RenderRequest req;
    RenderBasis basis;
    std::string err;
    bool cancelled = false;
    const std::size_t files = audio_files(r);
    const model::Snapshot before = r.doc->current();
    CHECK(!render(r, model::RenderKind::Freeze, 0, 0, nullptr, res, req, basis, err, &cancelled,
                  true));
    CHECK(cancelled);
    CHECK(audio_files(r) == files);
    CHECK(r.doc->current() == before);
    CHECK(r.session->transport().state() == TransportState::Stopped);
    return 0;
}

int refusals_are_said_before_anything_rolls() {
    Rig r;
    CHECK(make_rig(r, "render_t8"));
    std::string err;
    RenderRequest req;
    RenderAsk ask;
    ask.track = r.track;
    ask.sample_rate = kRate;
    // Process Selection needs a selection.
    CHECK(!plan_render(r.doc->current(), model::RenderKind::Process, ask, req, err));
    CHECK(err.find("select a range") != std::string::npos);
    ask.track = 999;
    CHECK(!plan_render(r.doc->current(), model::RenderKind::Freeze, ask, req, err));

    // The transport must be stopped; a take must not be open.
    ask.track = r.track;
    CHECK(plan_render(r.doc->current(), model::RenderKind::Freeze, ask, req, err));
    CHECK(req.tail == static_cast<model::Frames>(kRenderFreezeTailMs) * kRate / 1000);
    {
        Driver audio(*r.session);
        r.session->play();
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        RenderInPlace rt(*r.session, *r.ctl);
        CHECK(!rt.start(r.doc->current(), r.dir, req, nullptr, err));
        CHECK(err.find("stop the transport") != std::string::npos);
        r.session->stop_playback();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    return 0;
}

}  // namespace

int main() {
    if (freeze_prints_the_chain_aligned_to_the_sample()) return 1;
    if (a_selection_starts_where_it_was_asked()) return 1;
    if (an_unbound_chain_leaves_no_file()) return 1;
    if (a_missed_block_leaves_no_file()) return 1;
    if (an_edit_while_it_rendered_is_refused()) return 1;
    if (a_bypass_while_it_rendered_is_refused()) return 1;
    if (a_cancel_removes_the_file_and_changes_nothing()) return 1;
    if (refusals_are_said_before_anything_rolls()) return 1;
    std::printf("render_in_place: ok\n");
    return 0;
}
