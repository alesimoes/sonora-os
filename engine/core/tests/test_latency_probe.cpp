// SPDX-License-Identifier: GPL-3.0-or-later
//
// The round trip measured through a loopback and the takes compensated by
// it (ADR-0019), with no hardware: drivers/fake cables an output back into
// a capture port a set number of frames later, as a converter's delay
// would, and the session's LatencyProbe must find that figure to the
// frame, at any period, across an xrun, through the monitored-input path
// with and without plugin delay compensation, and allocating nothing in
// process(). Then the figure goes to work: a take of what the monitor bus
// played, recorded back through the cable, must land on the timeline frame
// it was played at, for a plain take and for a punch, and the probe's
// failures must say why.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// The heap counter (playback_test.h) on this thread, which runs the fake's
// periods and so Session::process().
#define PLAYBACK_TEST_COUNT_HEAP 1
#include "playback_test.h"
#include "sonora/fake_backend.h"
#include "sonora/insert_chain_runner.h"
#include "sonora/latency_probe.h"
#include "sonora/plan_graph.h"
#include "sonora/session.h"
#include "sonora/wav_reader.h"

namespace {

namespace fs = std::filesystem;
using playback_test::heap_untouched;
using playback_test::reset_heap_counts;
using sonora::FakeBackend;
using sonora::LatencyProbe;
using sonora::Session;
using sonora::Track;

constexpr uint32_t kRate = 48000;
constexpr uint32_t kWindow = 2048;

struct Rig {
    fs::path dir;
    std::unique_ptr<Session> session;
    std::unique_ptr<FakeBackend> audio;

    Rig(const std::string& name, int tracks, uint32_t quantum,
        std::vector<FakeBackend::Loopback> cables, uint32_t outputs = 3) {
        dir = fs::temp_directory_path() / ("sonora_test_latency_probe_" + name);
        fs::remove_all(dir);
        sonora::SessionConfig cfg;
        cfg.dir = dir.string();
        cfg.track_count = tracks;
        cfg.sample_rate = kRate;
        cfg.format = sonora::WavWriter::Format::Float32;
        cfg.ring_seconds = 1.0;
        cfg.playback.chunk_frames = 256;
        cfg.playback.window_seconds = 0.02;
        cfg.playback.pool_chunks = 64;
        cfg.playback.max_block_frames = 256;
        cfg.playback.max_delay_frames = 4096;
        session = std::make_unique<Session>(cfg);
        FakeBackend::Config fc;
        fc.quantum = quantum;
        fc.signal = FakeBackend::Signal::Impulses;
        fc.impulse_interval = 1000;
        fc.loopbacks = std::move(cables);
        fc.output_capture_frames = std::size_t{1} << 18;
        audio = std::make_unique<FakeBackend>(fc);
        std::string error;
        ok = audio->open({"sonora", static_cast<uint32_t>(tracks), outputs, kRate, ""}, *session,
                         error) &&
             audio->activate(error) &&
             audio->connect_inputs_in_order() == tracks;
        if (!ok) std::fprintf(stderr, "rig %s: %s\n", name.c_str(), error.c_str());
        // Nothing reaches the bus unless a test says so: every input
        // carries the fake's impulses.
        for (const auto& t : session->tracks()) t->set_monitor(Track::Monitor::Off);
    }
    ~Rig() {
        audio->deactivate();
        session.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // Starts a measurement and runs periods until its result, counting the
    // heap on this thread meanwhile.
    bool measure(const LatencyProbe::Settings& set, LatencyProbe::Result& r,
                 uint32_t max_blocks = 4000) {
        std::string error;
        if (!session->latency_probe().start(set, &error)) {
            std::fprintf(stderr, "probe refused: %s\n", error.c_str());
            return false;
        }
        bool got = false;
        playback_test::t_counting = true;
        for (uint32_t b = 0; b < max_blocks && !got; ++b) {
            audio->run_blocks(1);
            got = session->latency_probe().poll(r);
        }
        playback_test::t_counting = false;
        return got;
    }

    // Everything process() wrote to `output`, from the first period.
    std::vector<float> played(uint32_t output) {
        std::vector<float> v(std::size_t{1} << 18);
        v.resize(audio->read_output(output, v.data(), v.size()));
        return v;
    }

    bool ok = false;
};

LatencyProbe::Settings probe_on(uint32_t output, uint32_t input) {
    LatencyProbe::Settings s;
    s.output = output;
    s.input = input;
    s.window = kWindow;
    return s;
}

bool read_wav(const fs::path& path, std::vector<float>& out, uint64_t& time_reference) {
    sonora::WavReader r;
    std::string error;
    if (!r.open(path.string(), error) || r.channels() != 1) return false;
    out.assign(static_cast<std::size_t>(r.frames()), 0.0f);
    time_reference = r.time_reference();
    return r.read(0, r.frames(), out.data()) == r.frames();
}

// The cable's figure to the frame: whole periods, a converter's odd delay,
// a cable longer than the window's half, at several period sizes; and the
// probe's output silent between its impulses.
int test_measures_the_cable() {
    struct Case {
        uint32_t quantum;
        uint32_t frames;
    };
    for (const Case c : {Case{128, 256}, Case{128, 300}, Case{64, 1000}, Case{256, 549},
                         Case{32, 37}}) {
        Rig rig("cable", 3, c.quantum, {{2, 0, 2, c.frames}});
        CHECK(rig.ok);
        reset_heap_counts();
        LatencyProbe::Result r{};
        CHECK(rig.measure(probe_on(2, 2), r));
        CHECK(heap_untouched());
        if (r.outcome != LatencyProbe::Outcome::Measured || r.frames != c.frames) {
            std::fprintf(stderr, "quantum %u, cable %u: %s\n", c.quantum, c.frames,
                         LatencyProbe::describe(r, kRate).c_str());
        }
        CHECK(r.outcome == LatencyProbe::Outcome::Measured);
        CHECK(r.frames == c.frames);
        CHECK(r.pings == 3 && r.spread == 0 && r.restarts == 0);
        CHECK(std::fabs(r.peak - 0.1f) < 1e-6f);
        CHECK(r.noise == 0.0f);
        CHECK(!rig.session->latency_probe().running());
        // Three impulses at the probe's level, each after a window of
        // silence, and nothing else on its output.
        const std::vector<float> out = rig.played(2);
        uint32_t impulses = 0;
        for (std::size_t i = 0; i < out.size(); ++i) {
            if (out[i] == 0.0f) continue;
            CHECK(out[i] == 0.1f);
            CHECK(i % (2 * kWindow) == kWindow);
            ++impulses;
        }
        CHECK(impulses == 3);
    }
    return 0;
}

// Frames lost to an xrun mid-measurement restart the ping they hit, and
// the figure is still the cable's.
int test_xrun_restarts_the_ping() {
    Rig rig("xrun", 3, 128, {{2, 0, 2, 300}});
    CHECK(rig.ok);
    CHECK(rig.session->latency_probe().start(probe_on(2, 2)));
    CHECK(rig.audio->run_blocks(40) == 40);  // inside the second ping
    rig.audio->inject_xrun(1000);
    LatencyProbe::Result r{};
    for (int b = 0; b < 4000 && !rig.session->latency_probe().poll(r); ++b) {
        rig.audio->run_blocks(1);
    }
    CHECK(r.outcome == LatencyProbe::Outcome::Measured);
    CHECK(r.frames == 300 && r.restarts == 1 && r.pings == 3);
    return 0;
}

// No cable: the input's own signal is as loud before the impulse as after
// it, which is no arrival. Silence is none either. Neither is a figure.
int test_no_cable_is_no_signal() {
    {
        Rig rig("nocable", 3, 128, {});
        CHECK(rig.ok);
        LatencyProbe::Result r{};
        CHECK(rig.measure(probe_on(2, 2), r));
        CHECK(r.outcome == LatencyProbe::Outcome::NoSignal);
        CHECK(r.noise == 1.0f);
        CHECK(LatencyProbe::describe(r, kRate).find("cabled") != std::string::npos);
    }
    {
        // A cable from an output nothing plays on.
        Rig rig("silent", 3, 128, {{1, 0, 2, 300}});
        CHECK(rig.ok);
        LatencyProbe::Result r{};
        CHECK(rig.measure(probe_on(2, 2), r));
        CHECK(r.outcome == LatencyProbe::Outcome::NoSignal);
        CHECK(r.peak == 0.0f);
    }
    return 0;
}

// A port the backend does not have, a second start while one runs, bad
// settings and a cancel: each refused or said, never a figure.
int test_refusals_and_cancel() {
    Rig rig("refusals", 3, 128, {{2, 0, 2, 300}});
    CHECK(rig.ok);
    LatencyProbe& p = rig.session->latency_probe();
    LatencyProbe::Result r{};
    CHECK(rig.measure(probe_on(5, 2), r));
    CHECK(r.outcome == LatencyProbe::Outcome::NoPort);
    CHECK(rig.measure(probe_on(2, 7), r));
    CHECK(r.outcome == LatencyProbe::Outcome::NoPort);

    std::string why;
    LatencyProbe::Settings bad = probe_on(2, 2);
    bad.pings = 0;
    CHECK(!p.start(bad, &why) && why.find("pings") != std::string::npos);
    bad = probe_on(2, 2);
    bad.window = kRate + 1;
    CHECK(!p.start(bad, &why) && why.find("one second") != std::string::npos);
    bad = probe_on(2, 2);
    bad.level = 1.5f;
    CHECK(!p.start(bad, &why));

    CHECK(p.start(probe_on(2, 2)));
    CHECK(!p.start(probe_on(2, 2), &why) && why.find("already") != std::string::npos);
    CHECK(rig.audio->run_blocks(10) == 10);
    p.cancel();
    CHECK(rig.audio->run_blocks(1) == 1);
    CHECK(p.poll(r));
    CHECK(r.outcome == LatencyProbe::Outcome::Cancelled);
    // Cancelled, its output is the mix's again: silence here.
    const std::vector<float> out = rig.played(2);
    for (std::size_t i = 10 * 128; i < out.size(); ++i) CHECK(out[i] == 0.0f);
    // And the next measurement runs.
    CHECK(rig.measure(probe_on(2, 2), r));
    CHECK(r.outcome == LatencyProbe::Outcome::Measured && r.frames == 300);
    return 0;
}

// A chain that returns its input `latency_frames` late, as a host with a
// declared latency does; one slot.
class DelayRunner final : public sonora::InsertChainRunner {
public:
    void begin(uint64_t) noexcept SONORA_RT override {}
    void end() noexcept SONORA_RT override {}
    uint32_t block_frames() const noexcept SONORA_RT override { return 0; }
    void skip(const sonora::PlanNode&, float* const out[2], uint32_t frames) noexcept SONORA_RT override {
        for (uint32_t i = 0; i < frames; ++i) out[0][i] = out[1][i] = 0.0f;
    }
    void run(const sonora::PlanNode& node, const float* const in[2], float* const out[2],
             uint32_t frames, const sonora::ChainParamEvent*, uint32_t) noexcept SONORA_RT override {
        const uint32_t d = node.latency_frames;
        for (uint32_t i = 0; i < frames; ++i) {
            ring_[pos_] = in[0][i];
            out[0][i] = out[1][i] = ring_[(pos_ + kCap - d) % kCap];
            pos_ = (pos_ + 1) % kCap;
        }
    }

private:
    static constexpr uint32_t kCap = 8192;
    float ring_[kCap] = {};
    uint32_t pos_ = 0;
};

// Monitored input through the session, measured through two cables: the
// probe's out_3 into in_1, track 1 monitoring it onto the bus's out_1, and
// out_1 into in_2. The engine adds nothing to the monitored input but the
// plan's compensation, the delay playback gets too (ADR-0019 §3): 2 x the
// cable with no chain, plus 512 frames once another track's chain declares
// 512 and the plan delays the dry track's edge to match.
int test_monitored_input_arrives_with_the_plans_compensation() {
    constexpr uint32_t kCable = 300;
    Rig rig("monitored", 3, 128, {{2, 0, 0, kCable}, {0, 0, 1, kCable}});
    CHECK(rig.ok);
    Session& s = *rig.session;
    s.tracks()[0]->set_monitor(Track::Monitor::Input);
    LatencyProbe::Result r{};
    reset_heap_counts();
    CHECK(rig.measure(probe_on(2, 1), r));
    CHECK(heap_untouched());
    CHECK(r.outcome == LatencyProbe::Outcome::Measured);
    CHECK(r.frames == 2 * kCable);
    // The pan law's centre, -3 dB, on the way through the bus.
    CHECK(std::fabs(r.peak - 0.1f * std::pow(10.0f, -3.0f / 20.0f)) < 1e-4f);

    DelayRunner runner;
    s.set_insert_chains(&runner);
    sonora::ChainBinding chain;
    chain.active = true;
    chain.chain_id = 103;
    chain.channel_slot = 0;
    chain.param_store = 7;
    chain.node_state = 0;
    chain.latency_frames = 512;
    sonora::DelayLinePool pool(s.delay_lines());
    sonora::Compensation comp;
    sonora::PlaybackPlan* p =
        playback_test::chain_plan(s, {sonora::ChainBinding{}, sonora::ChainBinding{}, chain}, pool,
                                  &comp);
    CHECK(comp.compensation_frames == 512);
    CHECK(playback_test::publish_plan(s, p));
    reset_heap_counts();
    CHECK(rig.measure(probe_on(2, 1), r));
    CHECK(heap_untouched());
    CHECK(r.outcome == LatencyProbe::Outcome::Measured);
    CHECK(r.frames == 2 * kCable + 512);
    s.set_insert_chains(nullptr);
    return 0;
}

// The figure at work. Track 1 monitors in_1's impulses onto out_1, which a
// cable carries into in_2, where track 2 records it; a second cable of the
// same length, out_3 into in_3, is what the probe measures, as the
// operator cables a spare pair. With the measured L, track 2's file is
// what out_1 played, frame for frame, labelled with the timeline frame it
// was played at. Without it, the same take is the cable's length late.
int take_through_the_cable(bool compensated) {
    constexpr uint32_t kCable = 2 * 128 + 37;
    Rig rig(compensated ? "take_l" : "take_0", 3, 128,
            {{0, 0, 1, kCable}, {2, 0, 2, kCable}});
    CHECK(rig.ok);
    Session& s = *rig.session;
    CHECK(s.create_dirs());
    s.tracks()[0]->set_monitor(Track::Monitor::Input);
    LatencyProbe::Result r{};
    CHECK(rig.measure(probe_on(2, 2), r));
    CHECK(r.outcome == LatencyProbe::Outcome::Measured && r.frames == kCable);
    CHECK(s.set_input_latency(compensated ? r.frames : 0));
    CHECK(s.input_latency() == (compensated ? kCable : 0));

    const auto s0 = static_cast<std::size_t>(rig.audio->process_calls() * 128);
    s.disk().start();
    s.tracks()[1]->set_rec_armed(true);
    std::string error;
    CHECK(s.start_recording(1, &error));
    std::string refused;
    CHECK(!s.set_input_latency(10, &refused));
    CHECK(refused.find("during a take") != std::string::npos);
    CHECK(rig.audio->run_blocks(100) == 100);
    CHECK(s.take_origin() == 0);
    CHECK(s.stop(&error));
    const std::vector<Session::ClosedTake> closed = s.closed_takes();
    CHECK(closed.size() == 1 && closed[0].latency == (compensated ? kCable : 0));

    std::vector<float> rec;
    uint64_t tr = 0;
    CHECK(read_wav(rig.dir / "Audio Files" / "Audio 2_01.wav", rec, tr));
    CHECK(tr == 0);
    // Every frame the playhead covered, less the L it skipped at the start
    // (those line up with no timeline frame).
    CHECK(rec.size() == 100 * 128 - (compensated ? kCable : 0));
    const std::vector<float> out1 = rig.played(0);
    std::size_t impulses = 0;
    for (std::size_t j = 0; j < rec.size(); ++j) {
        // Timeline frame j was played at server frame s0 + j; uncompensated,
        // the file holds what was played the cable's length earlier.
        const float want = compensated ? out1[s0 + j] : out1[s0 + j - kCable];
        if (rec[j] != want) {
            std::fprintf(stderr, "%s take, frame %zu: %g, played %g\n",
                         compensated ? "compensated" : "uncompensated", j, rec[j], want);
            return 1;
        }
        if (want != 0.0f) ++impulses;
    }
    CHECK(impulses >= 10);
    return 0;
}

int test_takes_land_where_they_were_played() {
    CHECK(take_through_the_cable(true) == 0);
    CHECK(take_through_the_cable(false) == 0);
    return 0;
}

// A punch with L: its file covers exactly the timeline range its handles
// ask for, [A - H, B + H), lined up with what was played there; the capture
// ran L later on the playhead, so the take follows out only once the
// playhead has passed B + H + L.
int test_punch_covers_its_timeline_range() {
    constexpr uint32_t kCable = 300;
    Rig rig("punch", 3, 128, {{0, 0, 1, kCable}, {2, 0, 2, kCable}});
    CHECK(rig.ok);
    Session& s = *rig.session;
    CHECK(s.create_dirs());
    s.tracks()[0]->set_monitor(Track::Monitor::Input);
    LatencyProbe::Result r{};
    CHECK(rig.measure(probe_on(2, 2), r));
    CHECK(r.outcome == LatencyProbe::Outcome::Measured && r.frames == kCable);
    CHECK(s.set_input_latency(r.frames));

    const auto s0 = static_cast<std::size_t>(rig.audio->process_calls() * 128);
    s.disk().start();
    s.tracks()[1]->set_rec_armed(true);
    sonora::PunchRange range;
    range.monitor_in = 4000;
    range.monitor_out = 9000;
    range.capture_from = 3000;
    range.capture_to = 10000;
    std::string error;
    CHECK(s.start_punch(1, range, &error));
    bool followed = false;
    for (int b = 0; b < 200 && !followed; ++b) {
        CHECK(rig.audio->run_blocks(1) == 1);
        const uint64_t at = s.transport().playhead();
        followed = s.follow_punch_out();
        // Not before the capture's end on the playhead, B + H + L.
        if (followed) CHECK(at >= 10000 + kCable);
    }
    CHECK(followed);
    CHECK(s.take_origin() == 3000);
    CHECK(s.stop(&error));
    const std::vector<Session::ClosedTake> closed = s.closed_takes();
    CHECK(closed.size() == 1 && closed[0].latency == kCable && closed[0].origin == 3000);

    std::vector<float> rec;
    uint64_t tr = 0;
    CHECK(read_wav(rig.dir / "Audio Files" / "Audio 2_01.wav", rec, tr));
    CHECK(tr == 3000);
    CHECK(rec.size() == 7000);
    const std::vector<float> out1 = rig.played(0);
    std::size_t impulses = 0;
    for (std::size_t j = 0; j < rec.size(); ++j) {
        CHECK(rec[j] == out1[s0 + 3000 + j]);
        if (rec[j] != 0.0f) ++impulses;
    }
    CHECK(impulses == 7);
    return 0;
}

}  // namespace

int main() {
    if (test_measures_the_cable()) return 1;
    if (test_xrun_restarts_the_ping()) return 1;
    if (test_no_cable_is_no_signal()) return 1;
    if (test_refusals_and_cancel()) return 1;
    if (test_monitored_input_arrives_with_the_plans_compensation()) return 1;
    if (test_takes_land_where_they_were_played()) return 1;
    if (test_punch_covers_its_timeline_range()) return 1;
    std::printf("latency_probe: all tests passed\n");
    return 0;
}
