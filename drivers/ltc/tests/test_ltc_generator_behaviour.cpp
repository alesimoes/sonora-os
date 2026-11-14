// SPDX-License-Identifier: GPL-3.0-or-later
//
// LTC generate end to end through the fakes (ADR-0124 §5): a scripted engine
// publishes the playhead, the generator writes LTC into the fake sink, and the
// test decodes what was written and checks it against the published position.
// Everything advances by control tick and by sample count; nothing reads a
// clock or sleeps. The chase side lives in test_ltc_chase_behaviour.cpp.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ltc_test_helpers.h"

using namespace ltc_test;

namespace {

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
    generator_output_decodes_to_the_published_position();
    generator_resyncs_on_locate();
    generator_is_silent_when_stopped_and_counts_short_writes();
    if (failures == 0) std::printf("ltc generator behaviour: ok\n");
    return failures == 0 ? 0 : 1;
}
