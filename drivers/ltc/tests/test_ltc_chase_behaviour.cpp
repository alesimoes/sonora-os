// SPDX-License-Identifier: GPL-3.0-or-later
//
// LTC chase end to end through the fakes (ADR-0124 §4): a scripted engine
// publishes the transport and records the commands sent; the source is the
// fake of drivers/ltc/testing. Everything advances by control tick and by
// sample count; nothing reads a clock or sleeps. The generator side lives in
// test_ltc_generator_behaviour.cpp.
#include <cstdint>
#include <string>
#include <vector>

#include "ltc_test_helpers.h"

using namespace ltc_test;

namespace {

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
    if (failures == 0) std::printf("ltc chase behaviour: ok\n");
    return failures == 0 ? 0 : 1;
}
