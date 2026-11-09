// SPDX-License-Identifier: GPL-3.0-or-later
//
// LTC chase and generate end to end through the fakes (ADR-0124 §4, §5): a
// scripted engine publishes the transport and records the commands sent; the
// source and sink are the fakes of drivers/ltc/testing. Everything advances
// by control tick and by sample count; nothing reads a clock or sleeps.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "fake_ltc_io.h"
#include "sonora/api/engine_control.h"
#include "sonora/api/state_block.h"
#include "sonora/api/state_view.h"
#include "sonora/ltc/ltc.h"
#include "sonora/ltc/ltc_sync.h"

namespace {

int failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            ++failures;                                                  \
        }                                                                \
    } while (0)

namespace api = sonora::api;
using namespace sonora::ltc;
using api::TransportState;

constexpr std::uint32_t kSr = 48'000;
constexpr LtcRate kRate = LtcRate::Fps25;
constexpr std::size_t kFrameSamples = 1'920;   // 25 fps at 48 kHz, exact

// The engine as the drivers see it: a published transport (the real state
// block and view) and the commands they send.
class ScriptedEngine : public api::EngineControl {
public:
    explicit ScriptedEngine(bool attached = true) : memory_(1) {
        std::string error;
        writer_.init(memory_.data(), memory_.bytes(), 1, 0, 10'000'000);
        if (attached) view_.attach(memory_.data(), memory_.bytes(), error);
        set(TransportState::Stopped, 0);
    }

    void set(TransportState state, std::uint64_t playhead) {
        api::TransportFields t;
        t.state = state;
        t.playhead = playhead;
        writer_.publish(t, nullptr, nullptr);
    }

    bool connect(std::string&) override { return true; }
    const api::Hello& hello() const override { return hello_; }
    std::uint64_t send(const api::CommandRequest& r) override {
        commands.push_back(r);
        return ++seq_;
    }
    std::uint64_t send(const api::ParameterChange&) override { return ++seq_; }
    std::uint64_t send(const api::PluginParameterChange&) override { return ++seq_; }
    std::uint64_t submit(const api::ModelRequest&) override { return ++seq_; }
    void poll(api::Inbox&) override {}
    api::StateView& state() override { return view_; }

    std::size_t count(const char* id) const {
        return static_cast<std::size_t>(
            std::count_if(commands.begin(), commands.end(),
                          [id](const api::CommandRequest& c) { return c.command == id; }));
    }

    std::vector<api::CommandRequest> commands;

private:
    api::StateBlockMemory memory_;
    api::StateWriter writer_;
    api::StateView view_;
    api::Hello hello_;
    std::uint64_t seq_ = 0;
};

// A source the test loads one tick's samples into.
class PacedSource final : public LtcSampleSource {
public:
    void load(const std::vector<float>& samples) {
        pending_.assign(samples.begin(), samples.end());
        pos_ = 0;
    }
    std::size_t read(Span<float> out) override {
        const std::size_t n = std::min(out.size, pending_.size() - pos_);
        std::copy_n(pending_.data() + pos_, n, out.data);
        pos_ += n;
        return n;
    }

private:
    std::vector<float> pending_;
    std::size_t pos_ = 0;
};

LtcTime tc(int h, int m, int s, int f, LtcRate rate = kRate) {
    LtcTime t;
    t.h = static_cast<std::uint8_t>(h);
    t.m = static_cast<std::uint8_t>(m);
    t.s = static_cast<std::uint8_t>(s);
    t.frame = static_cast<std::uint8_t>(f);
    t.rate = rate;
    return t;
}

// Consecutive frames of timecode from `start`, the way a master sends them.
class Stream {
public:
    explicit Stream(const LtcTime& start) : enc_(kSr, kRate), next_(start) {}
    std::vector<float> frames(std::size_t n) {
        std::vector<float> out(n * kFrameSamples);
        next_ = enc_.render(next_, Span<float>{out.data(), out.size()});
        return out;
    }

private:
    LtcEncoder enc_;
    LtcTime next_;
};

LtcConfig chase_config() {
    LtcConfig c;
    c.mode = LtcMode::Chase;
    c.sample_rate = kSr;
    c.rate = kRate;
    return c;
}

LtcConfig generate_config() {
    LtcConfig c = chase_config();
    c.mode = LtcMode::Generate;
    return c;
}

std::uint64_t sample_of(const LtcTime& t) { return t.to_samples(kSr); }

struct Heard {
    std::vector<LtcTime> time;
    std::vector<std::size_t> end;
};

Heard decode(const std::vector<float>& signal) {
    Heard h;
    LtcDecoder dec(kSr, kRate);
    LtcDecoded out;
    constexpr std::size_t kBlock = 1024;
    for (std::size_t pos = 0; pos < signal.size(); pos += kBlock) {
        const std::size_t n = std::min(kBlock, signal.size() - pos);
        dec.feed(Span<const float>{signal.data() + pos, n}, out);
        for (std::size_t i = 0; i < out.count; ++i) {
            h.time.push_back(out.time[i]);
            h.end.push_back(pos + out.end_offset[i]);
        }
    }
    return h;
}

// ---------------------------------------------------------------- chase

// A stopped transport far from the timecode: the second of two consecutive
// frames locks and locates to the start of the frame after it, and the next
// frame, now inside the window, starts the transport. The offset moves the
// target by itself.
void chase_locates_on_first_lock() {
    for (const std::int64_t offset : {std::int64_t{0}, std::int64_t{96'000}}) {
        Stream stream(tc(1, 0, 0, 0));
        // Few frames: the playhead is read once per tick and does not move
        // inside it, so a long burst would rightly run ahead of the window.
        const std::vector<float> signal = stream.frames(5);
        const Heard ref = decode(signal);
        CHECK(ref.time.size() >= 3);
        if (ref.time.size() < 2) return;

        ScriptedEngine engine;
        FakeLtcSource source(signal, 480);
        LtcConfig config = chase_config();
        config.offset_samples = offset;
        LtcChaseDriver driver(engine, source, config);
        std::string why;
        CHECK(driver.start(kRate, why));
        driver.tick();

        CHECK(engine.commands.size() == 2);
        if (engine.commands.size() < 2) return;
        CHECK(engine.commands[0].command == "transport.locate");
        const LtcTime after_second =
            LtcTime::from_frames(ref.time[1].to_frames() + 1u, kRate);
        CHECK(engine.commands[0].argument ==
              std::to_string(sample_of(after_second) + static_cast<std::uint64_t>(offset)));
        CHECK(engine.commands[1].command == "transport.play");
        CHECK(driver.chase().locked());
    }
}

// One frame is not a lock: nothing moves until two arrive in step.
void a_lone_frame_does_not_lock() {
    Stream stream(tc(1, 0, 0, 0));
    ScriptedEngine engine;
    FakeLtcSource source(stream.frames(2), 480);
    LtcChaseDriver driver(engine, source, chase_config());
    std::string why;
    CHECK(driver.start(kRate, why));
    driver.tick();
    CHECK(engine.commands.empty());
    CHECK(!driver.chase().locked());
}

// A transport already rolling at the timecode is left alone as frames
// advance, tick after tick.
void chase_plays_while_frames_advance() {
    Stream stream(tc(1, 0, 0, 0));
    ScriptedEngine engine;
    PacedSource source;
    LtcChaseDriver driver(engine, source, chase_config());
    std::string why;
    CHECK(driver.start(kRate, why));

    source.load(stream.frames(5));
    driver.tick();
    CHECK(engine.count("transport.locate") == 1);
    CHECK(engine.count("transport.play") == 1);
    const std::size_t after_lock = engine.commands.size();

    for (int tick = 0; tick < 12; ++tick) {
        engine.set(TransportState::Rolling, driver.chase().target());
        source.load(stream.frames(1));
        driver.tick();
    }
    CHECK(engine.commands.size() == after_lock);
    CHECK(driver.chase().locked());
}

// The silence is counted in control ticks: the transport stops after the
// budget and not a tick before, once.
void chase_stops_after_the_silence_budget() {
    Stream stream(tc(1, 0, 0, 0));
    ScriptedEngine engine;
    PacedSource source;
    const LtcConfig config = chase_config();
    LtcChaseDriver driver(engine, source, config);
    std::string why;
    CHECK(driver.start(kRate, why));
    source.load(stream.frames(5));
    driver.tick();
    engine.set(TransportState::Rolling, driver.chase().target());
    source.load({});

    std::uint32_t stopped_at = 0;
    for (std::uint32_t tick = 1; tick <= config.silence_ticks + 4; ++tick) {
        driver.tick();
        if (stopped_at == 0 && engine.count("transport.stop") > 0) stopped_at = tick;
    }
    // The first quiet tick may still be the one the last frames arrived in.
    CHECK(stopped_at >= config.silence_ticks);
    CHECK(stopped_at <= config.silence_ticks + 1);
    CHECK(engine.count("transport.stop") == 1);

    engine.set(TransportState::Stopped, driver.chase().target());
    for (int tick = 0; tick < 20; ++tick) driver.tick();
    CHECK(engine.count("transport.stop") == 1);
}

// A frame lost to a dropout costs neither a locate nor a stop: the chase
// takes the next two in step and carries on.
void chase_tolerates_a_dropped_frame() {
    const LtcTime start = tc(1, 0, 0, 0);
    const std::uint64_t base = sample_of(start);
    Stream stream(start);
    ScriptedEngine engine;
    PacedSource source;
    LtcChaseDriver driver(engine, source, chase_config());
    std::string why;
    CHECK(driver.start(kRate, why));

    for (std::size_t tick = 0; tick < 16; ++tick) {
        std::vector<float> frame = stream.frames(1);
        if (tick == 6) std::fill(frame.begin(), frame.end(), 0.0f);
        const bool rolling = engine.count("transport.play") > 0;
        engine.set(rolling ? TransportState::Rolling : TransportState::Stopped,
                   base + tick * kFrameSamples);
        source.load(frame);
        driver.tick();
    }
    CHECK(engine.count("transport.play") == 1);
    CHECK(engine.count("transport.locate") == 0);
    CHECK(engine.count("transport.stop") == 0);
    CHECK(driver.chase().locked());
}

// A take is never cut by a jump: the same far-off timecode that locates a
// rolling transport only counts as drift while recording.
void chase_never_locates_while_recording() {
    Stream stream(tc(1, 0, 0, 0));
    const std::vector<float> signal = stream.frames(9);

    {
        ScriptedEngine engine;
        engine.set(TransportState::Rolling, 0);
        FakeLtcSource source(signal, 480);
        LtcChaseDriver driver(engine, source, chase_config());
        std::string why;
        CHECK(driver.start(kRate, why));
        driver.tick();
        CHECK(engine.count("transport.locate") >= 1);   // the guard has something to guard
    }
    {
        ScriptedEngine engine;
        engine.set(TransportState::Recording, 0);
        FakeLtcSource source(signal, 480);
        LtcChaseDriver driver(engine, source, chase_config());
        std::string why;
        CHECK(driver.start(kRate, why));
        driver.tick();
        CHECK(engine.commands.empty());
        CHECK(driver.chase().drift() > 0);
    }
}

// A configuration at another rate than the session's is refused with a
// reason, and a refused driver does nothing: it does not even read.
void rate_mismatch_is_refused() {
    Stream stream(tc(1, 0, 0, 0));
    ScriptedEngine engine;
    FakeLtcSource source(stream.frames(9), 480);
    LtcChaseDriver driver(engine, source, chase_config());
    std::string why;
    CHECK(!driver.start(LtcRate::Fps30, why));
    CHECK(!why.empty());
    driver.tick();
    CHECK(engine.commands.empty());
    CHECK(source.reads() == 0);

    LtcChaseDriver wrong_mode(engine, source, generate_config());
    why.clear();
    CHECK(!wrong_mode.start(kRate, why));
    CHECK(!why.empty());

    why.clear();
    CHECK(driver.start(kRate, why));   // the right rate starts the same driver
}

// With no transport to compare against, silence is not evidence: the chase
// must not stop a transport it cannot see.
void chase_waits_for_a_transport() {
    ScriptedEngine engine(/*attached=*/false);
    PacedSource source;
    LtcChaseDriver driver(engine, source, chase_config());
    std::string why;
    CHECK(driver.start(kRate, why));
    Stream stream(tc(1, 0, 0, 0));
    source.load(stream.frames(5));
    driver.tick();
    for (int tick = 0; tick < 20; ++tick) driver.tick();
    CHECK(engine.commands.empty());
}

// ---------------------------------------------------------------- generate

constexpr std::size_t kStep = 4'800;   // 100 ms of playhead per tick

// Every decoded frame sits where the published position says: the frame
// ended at `position(end)`, which is the start of the next label, to within
// a couple of bit cells.
std::size_t check_against_position(const Heard& heard, std::size_t from, std::size_t to,
                                   std::uint64_t (*position)(std::size_t, std::uint64_t, std::size_t),
                                   std::uint64_t a, std::size_t b) {
    std::size_t checked = 0;
    for (std::size_t i = 0; i < heard.time.size(); ++i) {
        const std::size_t e = heard.end[i];
        if (e < from || e >= to) continue;
        const std::uint64_t edge = sample_of(
            LtcTime::from_frames(heard.time[i].to_frames() + 1u, kRate));
        const std::uint64_t at = position(e, a, b);
        const std::uint64_t gap = at > edge ? at - edge : edge - at;
        CHECK(gap <= 48);
        ++checked;
    }
    return checked;
}

std::uint64_t linear_position(std::size_t index, std::uint64_t origin, std::size_t) {
    return origin + index;
}
std::uint64_t after_jump_position(std::size_t index, std::uint64_t origin, std::size_t j0) {
    return origin + (index - j0);
}

void generator_output_decodes_to_the_published_position() {
    const std::uint64_t p0 = sample_of(tc(0, 10, 0, 0)) + 500;   // mid-frame
    ScriptedEngine engine;
    FakeLtcSink sink(kSr * 4);
    LtcGenerator gen(engine, sink, generate_config());
    std::string why;
    CHECK(gen.start(kRate, why));

    engine.set(TransportState::Rolling, p0);
    gen.tick();
    CHECK(sink.samples().empty());   // the first tick only finds its place
    for (std::size_t i = 1; i <= 20; ++i) {
        engine.set(TransportState::Rolling, p0 + i * kStep);
        gen.tick();
    }
    CHECK(sink.samples().size() == 20 * kStep);
    CHECK(gen.short_writes() == 0);

    const Heard heard = decode(sink.samples());
    const std::size_t n = check_against_position(heard, 0, sink.samples().size(),
                                                 linear_position, p0, 0);
    CHECK(n >= 40);
}

// A locate restarts the timecode at the new place and the receiver re-syncs
// there, a long way off and behind the old position alike.
void generator_resyncs_on_locate() {
    const std::uint64_t p0 = sample_of(tc(0, 10, 0, 0)) + 500;
    const std::uint64_t p1 = sample_of(tc(2, 0, 0, 0)) + 300;
    ScriptedEngine engine;
    FakeLtcSink sink(kSr * 4);
    LtcGenerator gen(engine, sink, generate_config());
    std::string why;
    CHECK(gen.start(kRate, why));

    engine.set(TransportState::Rolling, p0);
    gen.tick();
    for (std::size_t i = 1; i <= 10; ++i) {
        engine.set(TransportState::Rolling, p0 + i * kStep);
        gen.tick();
    }
    const std::size_t j0 = sink.samples().size();
    CHECK(j0 == 10 * kStep);

    engine.set(TransportState::Rolling, p1);
    gen.tick();
    CHECK(sink.samples().size() == j0);   // the jump itself writes nothing
    for (std::size_t i = 1; i <= 10; ++i) {
        engine.set(TransportState::Rolling, p1 + i * kStep);
        gen.tick();
    }
    CHECK(sink.samples().size() == 20 * kStep);

    const Heard heard = decode(sink.samples());
    const std::size_t before = check_against_position(heard, 0, j0, linear_position, p0, 0);
    // The frame cut by the jump is not a frame; skip two frames past it.
    const std::size_t after = check_against_position(
        heard, j0 + 2 * kFrameSamples, sink.samples().size(), after_jump_position, p1, j0);
    CHECK(before >= 15);
    CHECK(after >= 15);

    // And a jump backwards, to a place before the old one, is the same.
    const std::uint64_t p2 = sample_of(tc(0, 0, 10, 0));
    engine.set(TransportState::Rolling, p2);
    gen.tick();
    engine.set(TransportState::Rolling, p2 + kStep);
    gen.tick();
    CHECK(sink.samples().size() == 21 * kStep);
}

void generator_is_silent_when_stopped_and_counts_short_writes() {
    {
        ScriptedEngine engine;
        FakeLtcSink sink(kSr);
        LtcGenerator gen(engine, sink, generate_config());
        std::string why;
        CHECK(gen.start(kRate, why));
        for (std::size_t i = 0; i < 5; ++i) {
            engine.set(TransportState::Stopped, i * kStep);
            gen.tick();
        }
        CHECK(sink.samples().empty());
    }
    {
        ScriptedEngine engine;
        FakeLtcSink sink(1'000);
        LtcGenerator gen(engine, sink, generate_config());
        std::string why;
        CHECK(gen.start(kRate, why));
        engine.set(TransportState::Rolling, 0);
        gen.tick();
        engine.set(TransportState::Rolling, kStep);
        gen.tick();
        CHECK(gen.short_writes() >= 1);
        CHECK(sink.samples().size() == 1'000);
    }
    {
        ScriptedEngine engine;
        FakeLtcSink sink(kSr);
        LtcGenerator gen(engine, sink, generate_config());
        std::string why;
        CHECK(!gen.start(LtcRate::Fps24, why));
        CHECK(!why.empty());
        engine.set(TransportState::Rolling, 0);
        gen.tick();
        engine.set(TransportState::Rolling, kStep);
        gen.tick();
        CHECK(sink.samples().empty());
    }
}

}  // namespace

int main() {
    chase_locates_on_first_lock();
    a_lone_frame_does_not_lock();
    chase_plays_while_frames_advance();
    chase_stops_after_the_silence_budget();
    chase_tolerates_a_dropped_frame();
    chase_never_locates_while_recording();
    rate_mismatch_is_refused();
    chase_waits_for_a_transport();
    generator_output_decodes_to_the_published_position();
    generator_resyncs_on_locate();
    generator_is_silent_when_stopped_and_counts_short_writes();
    if (failures == 0) std::printf("ltc sync behaviour: ok\n");
    return failures == 0 ? 0 : 1;
}
