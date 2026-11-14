// SPDX-License-Identifier: GPL-3.0-or-later
//
// Process Selection through one insert (ADR-0087 §4), RenderInPlace's half:
// the file is that insert's output alone, latency taken off, while the
// track's other insert keeps its place in the document; the other inserts are
// silenced by a render-scoped override, never by an edit, and lifted on every
// way out (done, cancelled, failed); a host that never confirms the override
// fails the render and leaves no file. The chain is a test runner standing
// for a host of two instances: instance 0 halves the signal, instance 1
// delays it by the declared latency, and each honours the bypass the session
// hands it as a host's last-value table would. The override is a fake
// RenderSolo posting that bypass through Session::post_plugin_param, as
// PluginHosting does, and reading it back from the runner.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "sonora/api/render.h"
#include "sonora/closed_take.h"
#include "sonora/insert_chain_runner.h"
#include "sonora/model/operations.h"
#include "sonora/model/render_in_place.h"
#include "sonora/playback_controller.h"
#include "sonora/plugin/protocol.h"
#include "sonora/render_in_place.h"
#include "sonora/render_solo.h"
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
constexpr int64_t kLength = 48000;
constexpr uint32_t kLatency = 300;
constexpr uint32_t kStore = 7;
constexpr float kAmp = 0.1f;

float sine_at(int64_t i) {
    return kAmp * std::sin(2.0f * 3.14159265f * static_cast<float>(i % 48) / 48.0f);
}

// Two instances in slot order: 0 halves (latency 0), 1 delays by kLatency.
// A bypassed instance passes its input, still delayed by its latency, as a
// host does (ADR-0012: a toggle never shifts timing).
class TwoInsertRunner final : public InsertChainRunner {
public:
    void begin(uint64_t) noexcept SONORA_RT override {}
    void end() noexcept SONORA_RT override {}
    uint32_t block_frames() const noexcept SONORA_RT override { return 0; }
    void skip(const PlanNode&, float* const out[2], uint32_t frames) noexcept SONORA_RT override {
        for (uint32_t i = 0; i < frames; ++i) out[0][i] = out[1][i] = 0.0f;
    }
    void run(const PlanNode& node, const float* const in[2], float* const out[2], uint32_t frames,
             const ChainParamEvent* events, uint32_t count) noexcept SONORA_RT override {
        for (uint32_t k = 0; k < count; ++k) {
            if (events[k].param == plugin::kBypassParam && events[k].instance < 2) {
                bypass_[events[k].instance].store(events[k].value, std::memory_order_relaxed);
            }
        }
        const bool halves = bypass_[0].load(std::memory_order_relaxed) == 0.0f;
        for (uint32_t i = 0; i < frames; ++i) {
            ring_[pos_] = halves ? in[0][i] * 0.5f : in[0][i];
            out[0][i] = ring_[(pos_ + kRing - node.latency_frames) % kRing];
            out[1][i] = out[0][i];
            pos_ = (pos_ + 1) % kRing;
        }
    }
    // What the "host" reads for an instance's bypass.
    float bypass(uint32_t instance) const { return bypass_[instance].load(std::memory_order_relaxed); }

private:
    static constexpr uint32_t kRing = 4096;
    float ring_[kRing] = {};
    uint32_t pos_ = 0;
    std::atomic<float> bypass_[2] = {};
};

// PluginHosting's part, against the runner: the other instance bypassed
// through the session's parameter ring, confirmed once the runner has it.
// `confirms` false is a host that never takes it.
class FakeSolo final : public RenderSolo {
public:
    FakeSolo(Session& s, TwoInsertRunner& host) : session_(s), host_(host) {}
    bool render_solo(model::Id track, model::Id insert) override {
        ++solos;
        asked_track = track;
        asked_insert = insert;
        if (!posted_) posted_ = post(0, 1.0f);
        return confirms && host_.bypass(0) == 1.0f && host_.bypass(1) == 0.0f;
    }
    void render_unsolo(model::Id track) override {
        ++unsolos;
        lifted_track = track;
        post(0, 0.0f);   // the document's bypass of the other insert
        posted_ = false;
    }
    bool confirms = true;
    int solos = 0;
    int unsolos = 0;
    model::Id asked_track = 0, asked_insert = 0, lifted_track = 0;

private:
    bool post(uint32_t instance, float value) {
        if (!confirms) return true;   // a host that drops it
        const PlaybackPlan* p = session_.plans().current();
        return p != nullptr &&
               session_.post_plugin_param(p->epoch, kStore, instance, plugin::kBypassParam, value);
    }
    Session& session_;
    TwoInsertRunner& host_;
    bool posted_ = false;
};

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
    TwoInsertRunner runner;
    model::Id track = 0;
    model::Id other = 0;    // slot 0, halves
    model::Id target = 0;   // slot 1, delays
    std::unique_ptr<PlaybackController> ctl;
};

bool add_insert(Rig& r, uint32_t slot, const char* uri, model::Id& id, std::string& err) {
    model::ReplicationUpdate u;
    model::AddInsert in;
    in.track = r.track;
    in.insert = r.doc->current().next_id;
    in.slot = slot;
    in.plugin = {model::PluginFormat::Lv2, uri, uri, "1"};
    id = in.insert;
    return r.doc->apply(in, u, err);
}

bool make_rig(Rig& r, const std::string& dir) {
    fs::remove_all(dir);
    r.dir = dir;
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
    if (!add_insert(r, 0, "urn:test:half", r.other, err)) return false;
    if (!add_insert(r, 1, "urn:test:delay", r.target, err)) return false;

    r.session->reader().start();
    r.session->set_insert_chains(&r.runner);
    r.ctl = std::make_unique<PlaybackController>(*r.session);
    r.ctl->set_chain_resolver([](const model::TrackEdit&, uint32_t, ChainBinding& b) {
        b.channel_slot = 0;
        b.channel_generation = 0;
        b.param_store = kStore;
        b.node_state = 0;
        b.latency_frames = kLatency;
    });
    if (!r.ctl->show(r.doc->current(), err)) return false;
    if (!r.ctl->settle(5.0)) return false;
    r.ctl->apply_mix(r.doc->current());
    return true;
}

RenderAsk ask_for(const Rig& r, model::Id only, uint64_t from, uint64_t to) {
    RenderAsk ask;
    ask.track = r.track;
    ask.start = from;
    ask.end = to;
    ask.tail_ms = 0;
    ask.only_insert = only;
    ask.sample_rate = kRate;
    return ask;
}

// Starts `req` and drives the control loop until it ends, bounded by the
// render's progress (test_render_in_place's rule). False with `err` when it
// failed or never started.
bool run(Rig& r, const RenderRequest& req, RenderSolo* solo, RenderResult& res, RenderBasis& basis,
         std::string& err, bool cancel_soon = false, bool* cancelled = nullptr) {
    RenderInPlace rt(*r.session, *r.ctl);
    rt.set_solo(solo);
    bool was_cancelled = false;
    {
        Driver audio(*r.session);
        if (!rt.start(r.doc->current(), r.dir, req, nullptr, err)) return false;
        if (cancel_soon) {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            rt.cancel();
        }
        constexpr double kStalled = 20.0;
        uint64_t head = r.session->transport().playhead();
        double done = rt.progress();
        auto moved = std::chrono::steady_clock::now();
        for (;;) {
            r.ctl->pump();
            if (rt.poll(res, err, was_cancelled)) break;
            const uint64_t h = r.session->transport().playhead();
            const double p = rt.progress();
            const auto now = std::chrono::steady_clock::now();
            if (h != head || p != done) {
                head = h;
                done = p;
                moved = now;
            } else if (std::chrono::duration<double>(now - moved).count() > kStalled) {
                err = "the render never ended";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
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

// The lifted override reached the runner: bounded by audio blocks, not time.
bool other_restored(Rig& r) {
    Driver audio(*r.session);
    const uint64_t from = r.session->plans().blocks();
    while (r.session->plans().blocks() - from < 2000) {
        if (r.runner.bypass(0) == 0.0f) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

int the_file_is_the_one_insert_alone() {
    Rig r;
    CHECK(make_rig(r, "render_solo_t1"));
    FakeSolo solo(*r.session, r.runner);
    std::string err;
    RenderRequest req;
    CHECK_OK(plan_render(r.doc->current(), model::RenderKind::Process,
                         ask_for(r, r.target, 12000, 30000), req, err),
             err);
    CHECK(req.only_insert == r.target);
    const model::Snapshot before = r.doc->current();
    RenderResult res;
    RenderBasis basis;
    CHECK_OK(run(r, req, &solo, res, basis, err), err);
    CHECK(solo.asked_track == r.track && solo.asked_insert == r.target);
    CHECK(solo.unsolos == 1 && solo.lifted_track == r.track);
    // No edit: the document, its bypass included, is what it was.
    CHECK(r.doc->current() == before);
    CHECK(insert_bypass(r.doc->current(), *r.doc->current().edit->tracks[0]) ==
          insert_bypass(before, *before.edit->tracks[0]));

    // The halving insert played no part; the delay's latency is taken off.
    CHECK(res.frames == 18000 && res.start == 12000);
    WavReader rd;
    CHECK(rd.open(r.dir + "/" + res.relative_path, err));
    std::vector<float> d(18000);
    CHECK(rd.read(0, 18000, d.data()) == 18000);
    for (int64_t i = 0; i < 18000; ++i) {
        if (d[static_cast<std::size_t>(i)] != sine_at(12000 + i)) {
            std::fprintf(stderr, "frame %lld: %g, wanted %g\n", static_cast<long long>(i),
                         static_cast<double>(d[static_cast<std::size_t>(i)]),
                         static_cast<double>(sine_at(12000 + i)));
            return 1;
        }
    }
    rd.close();
    CHECK(other_restored(r));

    // Only the printed insert goes inactive; the other stays as it was.
    std::vector<model::ReplicationUpdate> updates;
    CHECK_OK(place_render(*r.doc, r.dir, req, basis, res, updates, err), err);
    const model::TrackEdit& t = *r.doc->current().edit->tracks[0];
    CHECK(t.inserts.size() == 2);
    for (const model::Insert& in : t.inserts) {
        CHECK(in.active == (in.id == r.other));
    }
    const model::InsertMix* m = r.doc->current().mix.find_insert(r.other);
    CHECK(m == nullptr || !m->bypass);
    return 0;
}

int an_unconfirmed_override_fails_with_no_file() {
    Rig r;
    CHECK(make_rig(r, "render_solo_t2"));
    FakeSolo solo(*r.session, r.runner);
    solo.confirms = false;
    std::string err;
    RenderRequest req;
    CHECK_OK(plan_render(r.doc->current(), model::RenderKind::Process,
                         ask_for(r, r.target, 0, 24000), req, err),
             err);
    const std::size_t files = audio_files(r);
    const model::Snapshot before = r.doc->current();
    RenderResult res;
    RenderBasis basis;
    CHECK(!run(r, req, &solo, res, basis, err));
    CHECK(err.find("did not confirm") != std::string::npos);
    CHECK(solo.solos > 1);   // asked again until the deadline, not once
    CHECK(solo.unsolos == 1);
    CHECK(audio_files(r) == files);
    CHECK(r.doc->current() == before);
    CHECK(r.session->transport().state() == TransportState::Stopped);
    return 0;
}

int a_cancel_lifts_the_override() {
    Rig r;
    CHECK(make_rig(r, "render_solo_t3"));
    FakeSolo solo(*r.session, r.runner);
    std::string err;
    RenderRequest req;
    CHECK_OK(plan_render(r.doc->current(), model::RenderKind::Process,
                         ask_for(r, r.target, 0, 48000), req, err),
             err);
    const std::size_t files = audio_files(r);
    const model::Snapshot before = r.doc->current();
    RenderResult res;
    RenderBasis basis;
    bool cancelled = false;
    CHECK(!run(r, req, &solo, res, basis, err, true, &cancelled));
    CHECK(cancelled);
    CHECK(solo.unsolos == 1);
    CHECK(audio_files(r) == files);
    CHECK(r.doc->current() == before);
    CHECK(other_restored(r));
    return 0;
}

int a_render_dropped_mid_roll_lifts_the_override() {
    Rig r;
    CHECK(make_rig(r, "render_solo_t4"));
    FakeSolo solo(*r.session, r.runner);
    std::string err;
    RenderRequest req;
    CHECK_OK(plan_render(r.doc->current(), model::RenderKind::Process,
                         ask_for(r, r.target, 0, 48000), req, err),
             err);
    const std::size_t files = audio_files(r);
    {
        Driver audio(*r.session);
        RenderInPlace rt(*r.session, *r.ctl);
        rt.set_solo(&solo);
        CHECK_OK(rt.start(r.doc->current(), r.dir, req, nullptr, err), err);
        CHECK(solo.unsolos == 0);
        // Server teardown: the job goes without a poll.
    }
    CHECK(solo.unsolos == 1);
    CHECK(audio_files(r) == files);
    CHECK(other_restored(r));
    return 0;
}

int one_insert_refusals() {
    Rig r;
    CHECK(make_rig(r, "render_solo_t5"));
    const model::Snapshot doc = r.doc->current();
    std::string err;
    RenderRequest req;
    // Freeze and Commit print the whole chain.
    CHECK(!plan_render(doc, model::RenderKind::Freeze, ask_for(r, r.target, 0, 0), req, err));
    CHECK(err.find("only Process Selection") != std::string::npos);
    CHECK(!plan_render(doc, model::RenderKind::Commit, ask_for(r, r.target, 0, 0), req, err));
    // Not an insert of the track.
    CHECK(!plan_render(doc, model::RenderKind::Process, ask_for(r, 999, 0, 24000), req, err));
    CHECK(err.find("not an active insert") != std::string::npos);
    // A bypassed insert would print the dry signal.
    model::ReplicationUpdate u;
    CHECK(r.doc->apply(model::SetInsertBypass{r.target, true}, u, err));
    CHECK(!plan_render(r.doc->current(), model::RenderKind::Process,
                       ask_for(r, r.target, 0, 24000), req, err));
    CHECK(err.find("bypassed") != std::string::npos);
    CHECK(r.doc->apply(model::SetInsertBypass{r.target, false}, u, err));

    // An engine without the override refuses rather than print the chain.
    CHECK_OK(plan_render(r.doc->current(), model::RenderKind::Process,
                         ask_for(r, r.target, 0, 24000), req, err),
             err);
    const std::size_t files = audio_files(r);
    RenderResult res;
    RenderBasis basis;
    CHECK(!run(r, req, nullptr, res, basis, err));
    CHECK(err.find("cannot silence") != std::string::npos);
    CHECK(audio_files(r) == files);
    // A request start() is handed for Freeze with one insert is refused too.
    FakeSolo solo(*r.session, r.runner);
    RenderRequest freeze = req;
    freeze.kind = model::RenderKind::Freeze;
    CHECK(!run(r, freeze, &solo, res, basis, err));
    CHECK(solo.solos == 0 && audio_files(r) == files);
    return 0;
}

int the_wire_options_become_the_ask() {
    api::RenderOptions o;
    o.track = 4;
    o.start = 100;
    o.end = 900;
    o.tail_ms = 250;
    o.only_insert = 17;
    o.name = "Vox Air";
    const RenderAsk a = RenderAsk::from_options(o, 44100);
    CHECK(a.track == 4 && a.start == 100 && a.end == 900 && a.tail_ms == 250);
    CHECK(a.only_insert == 17 && a.name == "Vox Air" && a.sample_rate == 44100);
    const RenderAsk whole = RenderAsk::from_options(api::RenderOptions{}, 48000);
    CHECK(whole.only_insert == 0 && whole.tail_ms == -1);
    return 0;
}

}  // namespace

int main() {
    if (the_wire_options_become_the_ask()) return 1;
    if (the_file_is_the_one_insert_alone()) return 1;
    if (an_unconfirmed_override_fails_with_no_file()) return 1;
    if (a_cancel_lifts_the_override()) return 1;
    if (a_render_dropped_mid_roll_lifts_the_override()) return 1;
    if (one_insert_refusals()) return 1;
    std::printf("render_solo: ok\n");
    return 0;
}
