// SPDX-License-Identifier: GPL-3.0-or-later
// MTC chase and generation over a scripted port (ADR-0122). The engine is a
// real state block written by a StateWriter, so the drivers read the
// transport exactly as they would read the engine's; the commands they send
// are recorded. Time passes only through the samples a test hands to tick().
#include <cstdio>
#include <string>
#include <vector>

#include "fake_mtc_port.h"
#include "sonora/api/state_block.h"
#include "sonora/surfaces/mtc_sync.h"

using namespace sonora::surfaces;
namespace api = sonora::api;

namespace {
int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

using Bytes = std::vector<std::uint8_t>;
constexpr std::uint32_t kSr = 48000;
constexpr std::uint64_t kFrame25 = kSr / 25;  // 1920 samples

Timecode tc(int h, int m, int s, int f, FrameRate r = FrameRate::Fps25) {
    Timecode t;
    t.hours = static_cast<std::uint8_t>(h);
    t.minutes = static_cast<std::uint8_t>(m);
    t.seconds = static_cast<std::uint8_t>(s);
    t.frames = static_cast<std::uint8_t>(f);
    t.rate = r;
    return t;
}

class Engine : public api::EngineControl {
public:
    Engine() : memory_(1) {
        writer_.init(memory_.data(), memory_.bytes(), 1, 1, 10'000'000);
        std::string error;
        view_.attach(memory_.data(), memory_.bytes(), error);
        publish(api::TransportState::Stopped, 0);
    }
    void publish(api::TransportState s, std::uint64_t playhead) {
        api::TransportFields t;
        t.state = s;
        t.playhead = playhead;
        api::TrackFields track;
        float peak = 0;
        writer_.publish(t, &track, &peak);
    }
    bool connect(std::string&) override { return true; }
    const api::Hello& hello() const override { return hello_; }
    uint64_t send(const api::CommandRequest& r) override {
        commands.push_back(r);
        return ++seq_;
    }
    uint64_t send(const api::ParameterChange&) override { return ++seq_; }
    uint64_t send(const api::PluginParameterChange&) override { return ++seq_; }
    uint64_t submit(const api::ModelRequest&) override { return ++seq_; }
    void poll(api::Inbox&) override {}
    api::StateView& state() override { return view_; }

    std::size_t count(const std::string& id) const {
        std::size_t n = 0;
        for (const auto& c : commands) n += c.command == id ? 1 : 0;
        return n;
    }

    std::vector<api::CommandRequest> commands;

private:
    api::StateBlockMemory memory_;
    api::StateWriter writer_;
    api::StateView view_;
    api::Hello hello_;
    uint64_t seq_ = 0;
};

MtcSyncSettings settings(MtcMode mode, FrameRate rate = FrameRate::Fps25) {
    MtcSyncSettings s;
    s.mode = mode;
    s.rate = rate;
    s.sample_rate = kSr;
    s.offset_samples = offset_for_session_start(tc(1, 0, 0, 0, rate), kSr);
    return s;
}

void feed_full(FakeMtcPort& port, const Timecode& t) {
    std::uint8_t b[kMtcFullFrameBytes];
    MtcEncoder::full_frame(t, b);
    port.feed(b, sizeof(b));
}

void feed_cycles(FakeMtcPort& port, MtcEncoder& enc, int cycles) {
    for (int i = 0; i < cycles * 8; ++i) {
        std::uint8_t q[2];
        enc.next(q);
        port.feed(q, 2);
    }
}

void test_mode_is_checked_at_start() {
    Engine engine;
    FakeMtcPort port;
    std::string error;
    MtcChaseDriver wrong(settings(MtcMode::Generate), engine, port);
    CHECK(!wrong.start(error) && !error.empty() && !wrong.started());
    MtcGenerator wrong_gen(settings(MtcMode::Chase), engine, port);
    CHECK(!wrong_gen.start(error) && error.find("chase") != std::string::npos);

    MtcSyncSettings bad_rate = settings(MtcMode::Chase);
    bad_rate.sample_rate = 0;
    MtcChaseDriver chase(bad_rate, engine, port);
    CHECK(!chase.start(error) && error.find("sample rate") != std::string::npos);

    // An instance that refused does nothing at all, not a degraded chase.
    feed_full(port, tc(1, 0, 10, 0));
    chase.tick(kFrame25 * 10);
    CHECK(engine.commands.empty());
    MtcGenerator idle(bad_rate, engine, port);
    idle.tick(kFrame25);
    CHECK(port.sent.empty());

    const auto a = make_mtc_sync(settings(MtcMode::Chase), engine, port);
    const auto b = make_mtc_sync(settings(MtcMode::Generate), engine, port);
    CHECK(a && a->mode() == MtcMode::Chase && a->start(error) && error.empty());
    CHECK(b && b->mode() == MtcMode::Generate && b->start(error));
    MtcSyncSettings junk = settings(MtcMode::Chase);
    junk.mode = static_cast<MtcMode>(7);
    CHECK(!make_mtc_sync(junk, engine, port));
}

// A full frame at 01:00:10:00, with the session starting at 01:00:00:00,
// is engine sample 10 s; the argument is that sample, as the UI sends it.
void test_chase_locates_with_offset() {
    Engine engine;
    FakeMtcPort port;
    port.set_read_chunk(3);
    std::string error;
    MtcChaseDriver chase(settings(MtcMode::Chase), engine, port);
    CHECK(chase.start(error));
    feed_full(port, tc(1, 0, 10, 0));
    chase.tick(0);
    CHECK(engine.commands.size() == 1);
    CHECK(engine.commands[0].command == "transport.locate");
    CHECK(engine.commands[0].argument == std::to_string(10 * kSr));
    CHECK(chase.stats().locates == 1);

    // Before the session's start there is nowhere to go.
    feed_full(port, tc(0, 59, 0, 0));
    chase.tick(0);
    CHECK(engine.commands.size() == 1 && chase.stats().out_of_range == 1);
}

// Parked on the master's position, two cycles of a rolling master: one play.
void test_chase_plays_when_frames_advance() {
    Engine engine;
    FakeMtcPort port;
    std::string error;
    MtcChaseDriver chase(settings(MtcMode::Chase), engine, port);
    CHECK(chase.start(error));
    MtcEncoder enc;
    enc.start(tc(1, 0, 4, 0));
    // The first cycle reports 01:00:04:02; the engine waits there.
    engine.publish(api::TransportState::Stopped, 4 * kSr + 2 * kFrame25);
    feed_cycles(port, enc, 1);
    chase.tick(2 * kFrame25);
    CHECK(engine.commands.empty());
    feed_cycles(port, enc, 1);
    chase.tick(2 * kFrame25);
    CHECK(engine.count("transport.play") == 1 && engine.commands.size() == 1);
    CHECK(chase.locked());
}

// The take is the thing that must not be lost: off by seconds while
// recording, the chase counts the drift and sends no locate.
void test_chase_never_locates_while_recording() {
    Engine engine;
    FakeMtcPort port;
    std::string error;
    MtcChaseDriver chase(settings(MtcMode::Chase), engine, port);
    CHECK(chase.start(error));
    engine.publish(api::TransportState::Recording, 0);
    MtcEncoder enc;
    enc.start(tc(1, 0, 30, 0));
    for (int i = 0; i < 4; ++i) {
        feed_cycles(port, enc, 1);
        chase.tick(2 * kFrame25);
    }
    feed_full(port, tc(1, 5, 0, 0));
    chase.tick(0);
    CHECK(engine.count("transport.locate") == 0);
    CHECK(engine.commands.empty());
    CHECK(chase.stats().drift_events == 5);
    CHECK(chase.stats().max_drift_samples == 300ull * kSr);

    // The same drift merely rolling is chased.
    engine.publish(api::TransportState::Rolling, 0);
    feed_full(port, tc(1, 0, 30, 0));
    chase.tick(0);
    CHECK(engine.count("transport.locate") == 1);
}

// Silence is counted in frame periods from the samples each tick reports,
// sub-frame ticks included; three frames with no position stop the roll.
void test_chase_stops_on_silence() {
    Engine engine;
    FakeMtcPort port;
    std::string error;
    MtcChaseDriver chase(settings(MtcMode::Chase), engine, port);
    CHECK(chase.start(error));
    MtcEncoder enc;
    enc.start(tc(1, 0, 0, 0));
    engine.publish(api::TransportState::Rolling, 2 * kFrame25);
    feed_cycles(port, enc, 1);
    chase.tick(kFrame25);
    CHECK(engine.commands.empty());
    for (int i = 0; i < 5; ++i) chase.tick(kFrame25 / 2);  // 2.5 frames
    CHECK(engine.commands.empty());
    chase.tick(kFrame25 / 2);  // 3
    CHECK(engine.count("transport.stop") == 1);
    chase.tick(kFrame25 * 10);
    CHECK(engine.count("transport.stop") == 1);
    CHECK(!chase.locked());
}

// ADR-0225: a master gone silent mid-take is counted, never a stop; the
// same silence merely rolling still stops. Silence is fed as audio samples.
MtcSyncStats silence_after_lock(api::TransportState state, Engine& engine) {
    FakeMtcPort port;
    std::string error;
    MtcChaseDriver chase(settings(MtcMode::Chase), engine, port);
    CHECK(chase.start(error));
    MtcEncoder enc;
    enc.start(tc(1, 0, 0, 0));
    engine.publish(state, 2 * kFrame25);
    feed_cycles(port, enc, 1);
    chase.tick(kFrame25);
    for (int i = 0; i < 4; ++i) chase.tick(kFrame25);
    return chase.stats();
}

void test_chase_silence_never_stops_a_recording() {
    Engine recording;
    const MtcSyncStats r = silence_after_lock(api::TransportState::Recording, recording);
    CHECK(recording.count("transport.stop") == 0 && recording.commands.empty());
    CHECK(r.silence_while_recording == 1 && r.stops == 0);

    Engine rolling;
    const MtcSyncStats p = silence_after_lock(api::TransportState::Rolling, rolling);
    CHECK(rolling.count("transport.stop") == 1);
    CHECK(p.silence_while_recording == 0 && p.stops == 1);
}

std::vector<MtcEvent> decode_sent(const FakeMtcPort& port) {
    MtcDecoder dec;
    std::vector<MtcEvent> out;
    for (const auto& m : port.sent) {
        dec.feed(m.data(), m.size(), [&](const MtcEvent& e) { out.push_back(e); });
    }
    return out;
}

// Stopped: one full frame. Rolling: one quarter frame per quarter-frame
// period of the published playhead, piece 0 on an even frame.
void test_generator_cadence() {
    Engine engine;
    FakeMtcPort port;
    std::string error;
    MtcGenerator gen(settings(MtcMode::Generate), engine, port);
    CHECK(gen.start(error));
    gen.tick(0);
    CHECK(port.sent.size() == 1 && port.sent[0].size() == kMtcFullFrameBytes);
    CHECK(port.sent[0][5] == 0x21);  // 25 fps, hour 1
    gen.tick(0);
    CHECK(port.sent.size() == 1);

    // 480 samples per quarter frame at 25 fps and 48 kHz.
    for (std::uint64_t p = 0; p <= 8 * kFrame25; p += 240) {
        engine.publish(api::TransportState::Rolling, p);
        gen.tick(240);
    }
    CHECK(gen.stats().quarter_frames == 33);  // indices 0..32
    for (std::size_t i = 1; i < port.sent.size(); ++i) {
        CHECK(port.sent[i].size() == 2 && port.sent[i][0] == kMtcQuarterFrame);
        CHECK(static_cast<std::size_t>(port.sent[i][1] >> 4) == (i - 1) % 8);
    }
    const auto ev = decode_sent(port);
    CHECK(ev.size() == 5);
    CHECK(ev[0].kind == MtcEvent::Kind::FullFrame && ev[0].position == tc(1, 0, 0, 0));
    for (std::size_t i = 1; i < ev.size(); ++i) {
        CHECK(ev[i].position == tc(1, 0, 0, static_cast<int>(2 * i)));
    }

    // Stop: a full frame where it stopped.
    engine.publish(api::TransportState::Stopped, 8 * kFrame25 + 100);
    gen.tick(0);
    CHECK(port.sent.back().size() == kMtcFullFrameBytes && port.sent.back()[8] == 8);
    // Locate while stopped: another.
    const std::size_t n = port.sent.size();
    engine.publish(api::TransportState::Stopped, 60 * kSr);
    gen.tick(0);
    CHECK(port.sent.size() == n + 1 && port.sent.back()[6] == 1);  // minute 1
    CHECK(gen.stats().full_frames == 3);
}

// Starting to roll mid-cycle waits for the next even frame, rather than
// bursting out pieces that are already late; a jump while rolling resyncs.
void test_generator_start_and_jump() {
    Engine engine;
    FakeMtcPort port;
    std::string error;
    MtcGenerator gen(settings(MtcMode::Generate), engine, port);
    CHECK(gen.start(error));
    engine.publish(api::TransportState::Stopped, kFrame25 + 10);
    gen.tick(0);
    engine.publish(api::TransportState::Rolling, kFrame25 + 10);
    gen.tick(0);
    CHECK(port.sent.size() == 1 && gen.stats().quarter_frames == 0);
    engine.publish(api::TransportState::Rolling, 2 * kFrame25);
    gen.tick(0);
    CHECK(gen.stats().quarter_frames == 1 && port.sent.back()[1] == 0x02);  // frame 2

    engine.publish(api::TransportState::Rolling, 20 * kSr);
    gen.tick(0);
    CHECK(gen.stats().resyncs == 1 && gen.stats().full_frames == 2);
    CHECK(port.sent.back().size() == 2 && (port.sent.back()[1] >> 4) == 0);
}

// A refused full frame is sent again on the next tick; a refused quarter
// frame is dropped, the stream carries on in time.
void test_generator_refused_writes() {
    Engine engine;
    FakeMtcPort port;
    std::string error;
    MtcGenerator gen(settings(MtcMode::Generate), engine, port);
    CHECK(gen.start(error));
    port.reject_writes(true);
    gen.tick(0);
    CHECK(port.sent.empty() && gen.stats().refused_writes == 1);
    port.reject_writes(false);
    gen.tick(0);
    CHECK(port.sent.size() == 1 && gen.stats().full_frames == 1);
}

// Drop-frame across a minute: the generator's own stream, decoded, skips
// labels ;00 and ;01 at 01:01:00 and its positions follow the playhead.
void test_generator_drop_frame_roundtrip() {
    Engine engine;
    FakeMtcPort port;
    std::string error;
    const MtcSyncSettings s = settings(MtcMode::Generate, FrameRate::Fps2997Drop);
    MtcGenerator gen(s, engine, port);
    CHECK(gen.start(error));
    const Timecode from = tc(1, 0, 59, 20, FrameRate::Fps2997Drop);
    const std::int64_t start =
        static_cast<std::int64_t>(to_samples(from, kSr)) + s.offset_samples;
    for (std::int64_t p = start; p < start + 30 * 1602; p += 400) {
        engine.publish(api::TransportState::Rolling, static_cast<std::uint64_t>(p));
        gen.tick(400);
    }
    const auto ev = decode_sent(port);
    bool crossed = false;
    for (const auto& e : ev) {
        CHECK(valid(e.position));
        if (e.position.minutes == 1 && e.position.seconds == 0) {
            CHECK(e.position.frames >= 2);
            crossed = true;
        }
    }
    CHECK(crossed);
}

// The generator's stream chases: a slave fed its bytes locates to it.
void test_generate_into_chase() {
    Engine master_engine;
    Engine slave_engine;
    FakeMtcPort wire;
    std::string error;
    MtcGenerator gen(settings(MtcMode::Generate), master_engine, wire);
    MtcChaseDriver chase(settings(MtcMode::Chase), slave_engine, wire);
    CHECK(gen.start(error) && chase.start(error));
    master_engine.publish(api::TransportState::Stopped, 30 * kSr);
    gen.tick(0);
    for (const auto& m : wire.sent) wire.feed(m.data(), m.size());
    chase.tick(0);
    CHECK(slave_engine.commands.size() == 1);
    CHECK(slave_engine.commands[0].argument == std::to_string(30 * kSr));
}

// 8 quarter frames per 2 frames at every rate: over 4 frames of playhead,
// 17 pieces (two whole cycles and the first piece of a third), decoding to
// frames 2 and 4 of the same rate.
void test_generator_rates() {
    const FrameRate rates[] = {FrameRate::Fps24, FrameRate::Fps25, FrameRate::Fps2997Drop,
                               FrameRate::Fps30};
    for (const FrameRate r : rates) {
        Engine engine;
        FakeMtcPort port;
        std::string error;
        MtcGenerator gen(settings(MtcMode::Generate, r), engine, port);
        CHECK(gen.start(error));
        gen.tick(0);  // stopped: the full frame at the hour
        const std::uint64_t end = to_samples(tc(1, 0, 0, 4, r), kSr) - to_samples(tc(1, 0, 0, 0, r), kSr);
        for (std::uint64_t p = 0; p <= end; p += 50) {
            engine.publish(api::TransportState::Rolling, p);
            gen.tick(50);
        }
        engine.publish(api::TransportState::Rolling, end + 1);  // exactly past the piece
        gen.tick(1);
        CHECK(gen.stats().quarter_frames == 17);
        const auto ev = decode_sent(port);
        CHECK(ev.size() == 3);
        if (ev.size() == 3) {
            CHECK(ev[1].position == tc(1, 0, 0, 2, r));
            CHECK(ev[2].position == tc(1, 0, 0, 4, r));
        }
    }
}

// A quarter frame lost on the wire costs the lock, not the roll: no stop and
// no locate, and the next whole cycle chases again.
void test_chase_tolerates_dropped_quarter_frame() {
    Engine engine;
    FakeMtcPort port;
    std::string error;
    MtcChaseDriver chase(settings(MtcMode::Chase), engine, port);
    CHECK(chase.start(error));
    MtcEncoder enc;
    enc.start(tc(1, 0, 4, 0));
    engine.publish(api::TransportState::Stopped, 4 * kSr + 2 * kFrame25);
    feed_cycles(port, enc, 2);
    chase.tick(4 * kFrame25);
    CHECK(engine.count("transport.play") == 1);
    engine.publish(api::TransportState::Rolling, 4 * kSr + 4 * kFrame25);

    for (int i = 0; i < 8; ++i) {  // one cycle with piece 3 missing
        std::uint8_t q[2];
        enc.next(q);
        if (i != 3) port.feed(q, 2);
    }
    chase.tick(2 * kFrame25);
    feed_cycles(port, enc, 1);
    engine.publish(api::TransportState::Rolling, 4 * kSr + 8 * kFrame25);
    chase.tick(2 * kFrame25);
    CHECK(engine.count("transport.stop") == 0);
    CHECK(engine.count("transport.locate") == 0);
    CHECK(engine.count("transport.play") == 1);
    CHECK(chase.locked());
}

// A master at another rate than the session's is counted and still chased
// (the sample position is right); the operator-facing refusal of a wrong
// mode or sample rate happens in start().
void test_chase_counts_rate_mismatch() {
    Engine engine;
    FakeMtcPort port;
    std::string error;
    MtcChaseDriver chase(settings(MtcMode::Chase, FrameRate::Fps25), engine, port);
    CHECK(chase.start(error));
    feed_full(port, tc(1, 0, 10, 0, FrameRate::Fps30));
    chase.tick(0);
    CHECK(chase.stats().rate_mismatches == 1);
    CHECK(engine.count("transport.locate") == 1);
}
}  // namespace

int main() {
    test_mode_is_checked_at_start();
    test_chase_locates_with_offset();
    test_chase_plays_when_frames_advance();
    test_chase_never_locates_while_recording();
    test_chase_stops_on_silence();
    test_chase_silence_never_stops_a_recording();
    test_generator_cadence();
    test_generator_start_and_jump();
    test_generator_refused_writes();
    test_generator_drop_frame_roundtrip();
    test_generate_into_chase();
    test_generator_rates();
    test_chase_tolerates_dropped_quarter_frame();
    test_chase_counts_rate_mismatch();
    if (failures == 0) std::printf("surface_mtc_sync: all passed\n");
    return failures == 0 ? 0 : 1;
}
