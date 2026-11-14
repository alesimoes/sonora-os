// SPDX-License-Identifier: GPL-3.0-or-later
//
// The LTC contract (ADR-0124): frame and sample maths at the four rates,
// the drop-frame rule, the chase policy and the fakes. Tick-driven, no
// clock. The codec itself is tested by test_ltc_codec.cpp.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include "fake_ltc_io.h"
#include "sonora/ltc/ltc.h"
#include "sonora/ltc/ltc_sync.h"

namespace {
std::atomic<std::uint64_t> g_allocations{0};
}

void* operator new(std::size_t n) {
    ++g_allocations;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

using sonora::api::TransportState;
using namespace sonora::ltc;

constexpr LtcRate kRates[] = {LtcRate::Fps24, LtcRate::Fps25, LtcRate::Fps2997Drop,
                              LtcRate::Fps30};
constexpr std::uint32_t kSampleRates[] = {44'100, 48'000, 96'000};

LtcTime tc(int h, int m, int s, int f, LtcRate rate) {
    LtcTime t;
    t.h = static_cast<std::uint8_t>(h);
    t.m = static_cast<std::uint8_t>(m);
    t.s = static_cast<std::uint8_t>(s);
    t.frame = static_cast<std::uint8_t>(f);
    t.rate = rate;
    return t;
}

// Every frame of the day maps to a valid label and back, labels rise in
// step, and the day wraps to zero.
int frames_round_trip() {
    for (const LtcRate rate : kRates) {
        LtcTime prev;
        for (std::uint64_t n = 0; n < frames_per_day(rate); ++n) {
            const LtcTime t = LtcTime::from_frames(n, rate);
            CHECK(t.valid());
            CHECK(t.rate == rate);
            CHECK(t.to_frames() == n);
            if (n > 0) {
                const bool later = t.h != prev.h ? t.h > prev.h
                                   : t.m != prev.m ? t.m > prev.m
                                   : t.s != prev.s ? t.s > prev.s
                                                   : t.frame > prev.frame;
                CHECK(later);
            }
            prev = t;
        }
        CHECK(LtcTime::from_frames(frames_per_day(rate), rate) == tc(0, 0, 0, 0, rate));
        CHECK(prev == tc(23, 59, 59, static_cast<int>(nominal_fps(rate)) - 1, rate));
    }
    CHECK(frames_per_day(LtcRate::Fps25) == 2'160'000u);
    CHECK(frames_per_day(LtcRate::Fps2997Drop) == 2'589'408u);
    return 0;
}

int drop_frame_vectors() {
    const LtcRate df = LtcRate::Fps2997Drop;
    // Minute 1 has no labels ;00 and ;01: 00:00:59;29 is followed by ;02.
    CHECK(tc(0, 0, 59, 29, df).to_frames() == 1799u);
    CHECK(tc(0, 1, 0, 2, df).to_frames() == 1800u);
    CHECK(LtcTime::from_frames(1800, df) == tc(0, 1, 0, 2, df));
    CHECK(!tc(0, 1, 0, 0, df).valid());
    CHECK(!tc(0, 1, 0, 1, df).valid());
    // Minute 10 keeps them: 9 minutes dropped 2 each.
    CHECK(tc(0, 9, 59, 29, df).to_frames() == 17'981u);
    CHECK(tc(0, 10, 0, 0, df).valid());
    CHECK(tc(0, 10, 0, 0, df).to_frames() == 17'982u);
    CHECK(LtcTime::from_frames(17'982, df) == tc(0, 10, 0, 0, df));
    CHECK(LtcTime::from_frames(17'983, df) == tc(0, 10, 0, 1, df));
    // One hour of drop frame is 107 892 frames: 3.6 s short of 108 000.
    CHECK(tc(1, 0, 0, 0, df).to_frames() == 107'892u);
    // Non-drop rates keep every label.
    CHECK(tc(0, 1, 0, 0, LtcRate::Fps30).valid());
    CHECK(tc(0, 1, 0, 0, LtcRate::Fps30).to_frames() == 1800u);
    CHECK(!tc(0, 0, 0, 24, LtcRate::Fps24).valid());
    CHECK(!tc(24, 0, 0, 0, LtcRate::Fps25).valid());
    return 0;
}

int samples_round_trip() {
    for (const LtcRate rate : kRates) {
        for (const std::uint32_t sr : kSampleRates) {
            const std::uint64_t day = frames_per_day(rate);
            for (std::uint64_t n = 0; n < day; n += 997) {
                const LtcTime t = LtcTime::from_frames(n, rate);
                const std::uint64_t at = t.to_samples(sr);
                CHECK(LtcTime::from_samples(at, sr, rate) == t);
                // The sample before the frame's first belongs to the frame before.
                if (n > 0) CHECK(LtcTime::from_samples(at - 1, sr, rate).to_frames() == n - 1);
            }
        }
    }
    // Exact values: one second of 25 fps at 48 kHz, and 29.97's 1601.6
    // samples per frame rounded up.
    CHECK(tc(0, 0, 1, 0, LtcRate::Fps25).to_samples(48'000) == 48'000u);
    CHECK(tc(0, 0, 0, 1, LtcRate::Fps2997Drop).to_samples(48'000) == 1602u);
    CHECK(LtcTime::from_samples(1601, 48'000, LtcRate::Fps2997Drop).to_frames() == 0u);
    CHECK(LtcTime::from_samples(1602, 48'000, LtcRate::Fps2997Drop).to_frames() == 1u);
    // An hour of drop frame labels is 107 892 * 1001 / 30 000 = 3599.9964 s.
    CHECK(tc(1, 0, 0, 0, LtcRate::Fps2997Drop).to_samples(48'000) == 172'799'828u);
    return 0;
}

LtcConfig chase_config() {
    LtcConfig c;
    c.mode = LtcMode::Chase;
    c.sample_rate = 48'000;
    c.rate = LtcRate::Fps25;   // 1920 samples a frame
    c.window_frames = 2;
    c.silence_ticks = 3;
    return c;
}

// Where the chase aims after frame `t`: the start of the frame after it.
std::uint64_t aim(const LtcTime& t) {
    return LtcTime::from_frames(t.to_frames() + 1, t.rate).to_samples(48'000);
}

int chase_locks_then_plays() {
    LtcChase chase(chase_config());
    CHECK(chase.window_samples() == 3840u);
    const LtcTime a = tc(1, 0, 0, 0, LtcRate::Fps25);
    const LtcTime b = LtcTime::from_frames(a.to_frames() + 1, LtcRate::Fps25);
    const LtcTime c = LtcTime::from_frames(a.to_frames() + 2, LtcRate::Fps25);

    chase.observe(0, TransportState::Stopped);
    CHECK(chase.on_time(a) == ChaseAction::None);   // one frame is not a lock
    CHECK(!chase.locked());
    CHECK(chase.on_time(b) == ChaseAction::Locate);
    CHECK(chase.locked());
    CHECK(chase.target() == aim(b));
    // Same tick: the locate is assumed taken, so the next frame plays.
    CHECK(chase.on_time(c) == ChaseAction::Play);
    CHECK(chase.on_tick() == ChaseAction::None);

    // Next tick, rolling along within the window: nothing to do.
    const LtcTime d = LtcTime::from_frames(c.to_frames() + 1, LtcRate::Fps25);
    chase.observe(aim(d) - 1000, TransportState::Rolling);
    CHECK(chase.on_time(d) == ChaseAction::None);
    // A jump in the timecode is not in step: it unlocks, then relocks.
    const LtcTime far = tc(2, 0, 0, 0, LtcRate::Fps25);
    const LtcTime far1 = LtcTime::from_frames(far.to_frames() + 1, LtcRate::Fps25);
    CHECK(chase.on_time(far) == ChaseAction::None);
    CHECK(!chase.locked());
    CHECK(chase.on_time(far1) == ChaseAction::Locate);
    CHECK(chase.target() == aim(far1));
    return 0;
}

int chase_window_edges() {
    const LtcTime a = tc(0, 0, 10, 0, LtcRate::Fps25);
    const LtcTime b = LtcTime::from_frames(a.to_frames() + 1, LtcRate::Fps25);
    const std::uint64_t at = aim(b);
    const std::int64_t offsets[] = {-3841, -3840, 0, 3840, 3841};
    for (const std::int64_t off : offsets) {
        LtcChase chase(chase_config());
        chase.observe(static_cast<std::uint64_t>(static_cast<std::int64_t>(at) + off),
                      TransportState::Rolling);
        chase.on_time(a);
        const bool inside = off >= -3840 && off <= 3840;
        CHECK(chase.on_time(b) == (inside ? ChaseAction::None : ChaseAction::Locate));
    }
    return 0;
}

int chase_never_locates_while_recording() {
    LtcChase chase(chase_config());
    chase.observe(0, TransportState::Recording);
    LtcTime t = tc(0, 5, 0, 0, LtcRate::Fps25);
    for (int i = 0; i < 10; ++i) {
        CHECK(chase.on_time(t) == ChaseAction::None);
        t = LtcTime::from_frames(t.to_frames() + 1, LtcRate::Fps25);
    }
    CHECK(chase.locked());
    CHECK(chase.drift() == 9u);   // every locked frame was out of the window
    return 0;
}

int chase_silence_budget() {
    LtcChase chase(chase_config());   // 3 silent ticks
    const LtcTime a = tc(0, 0, 0, 5, LtcRate::Fps25);
    const LtcTime b = LtcTime::from_frames(a.to_frames() + 1, LtcRate::Fps25);
    chase.observe(aim(b), TransportState::Rolling);
    chase.on_time(a);
    CHECK(chase.on_time(b) == ChaseAction::None);
    CHECK(chase.on_tick() == ChaseAction::None);   // this tick heard frames
    CHECK(chase.on_tick() == ChaseAction::None);   // silent 1
    CHECK(chase.on_tick() == ChaseAction::None);   // silent 2
    CHECK(chase.on_tick() == ChaseAction::Stop);   // silent 3: the budget
    CHECK(!chase.locked());
    CHECK(chase.on_tick() == ChaseAction::None);   // once, not every tick

    // A frame inside the budget restarts the count.
    LtcChase again(chase_config());
    again.observe(aim(b), TransportState::Rolling);
    again.on_time(a);
    again.on_time(b);
    again.on_tick();
    again.on_tick();
    again.on_tick();   // silent 2
    const LtcTime c = LtcTime::from_frames(b.to_frames() + 1, LtcRate::Fps25);
    CHECK(again.on_time(c) == ChaseAction::None);
    CHECK(again.on_tick() == ChaseAction::None);
    CHECK(again.on_tick() == ChaseAction::None);
    CHECK(again.on_tick() == ChaseAction::None);
    CHECK(again.on_tick() == ChaseAction::Stop);

    // Already stopped: silence asks for nothing.
    LtcChase stopped(chase_config());
    stopped.observe(0, TransportState::Stopped);
    stopped.on_time(a);
    for (int i = 0; i < 6; ++i) CHECK(stopped.on_tick() == ChaseAction::None);
    // Never heard anything: nothing either.
    LtcChase idle(chase_config());
    idle.observe(0, TransportState::Rolling);
    for (int i = 0; i < 6; ++i) CHECK(idle.on_tick() == ChaseAction::None);
    return 0;
}

int chase_rejects_and_offsets() {
    LtcChase chase(chase_config());
    chase.observe(0, TransportState::Stopped);
    CHECK(chase.on_time(tc(0, 0, 0, 0, LtcRate::Fps30)) == ChaseAction::None);
    CHECK(chase.on_time(tc(0, 0, 0, 25, LtcRate::Fps25)) == ChaseAction::None);
    CHECK(chase.rejected() == 2u);

    // Timecode before the session's start is not chased.
    LtcConfig c = chase_config();
    c.offset_samples = -48'000 * 60;
    LtcChase early(c);
    early.observe(0, TransportState::Stopped);
    early.on_time(tc(0, 0, 10, 0, LtcRate::Fps25));
    CHECK(early.on_time(tc(0, 0, 10, 1, LtcRate::Fps25)) == ChaseAction::None);
    CHECK(early.on_time(tc(0, 1, 10, 0, LtcRate::Fps25)) == ChaseAction::None);   // jump
    CHECK(early.on_time(tc(0, 1, 10, 1, LtcRate::Fps25)) == ChaseAction::Locate);
    CHECK(early.target() == aim(tc(0, 1, 10, 1, LtcRate::Fps25)) - 48'000u * 60u);
    return 0;
}

int chase_allocates_nothing() {
    LtcChase chase(chase_config());
    const std::uint64_t before = g_allocations.load();
    LtcTime t = tc(0, 0, 0, 0, LtcRate::Fps25);
    for (int i = 0; i < 1000; ++i) {
        chase.observe(static_cast<std::uint64_t>(i) * 1920u, TransportState::Rolling);
        chase.on_time(t);
        chase.on_tick();
        t = LtcTime::from_frames(t.to_frames() + 1, LtcRate::Fps25);
    }
    for (int i = 0; i < 10; ++i) chase.on_tick();
    chase.reset();
    CHECK(g_allocations.load() == before);
    return 0;
}

int config_validation() {
    std::string why;
    LtcConfig c = chase_config();
    CHECK(validate_ltc_config(c, LtcMode::Chase, LtcRate::Fps25, why));
    CHECK(!validate_ltc_config(c, LtcMode::Generate, LtcRate::Fps25, why));
    CHECK(!why.empty());
    why.clear();
    CHECK(!validate_ltc_config(c, LtcMode::Chase, LtcRate::Fps2997Drop, why));
    CHECK(why.find("rate") != std::string::npos);
    LtcConfig zero = c;
    zero.silence_ticks = 0;
    CHECK(!validate_ltc_config(zero, LtcMode::Chase, LtcRate::Fps25, why));
    LtcConfig gen = c;
    gen.mode = LtcMode::Generate;
    gen.silence_ticks = 0;   // a generator has no silence budget
    CHECK(validate_ltc_config(gen, LtcMode::Generate, LtcRate::Fps25, why));
    LtcConfig slow = c;
    slow.sample_rate = 100;
    CHECK(!validate_ltc_config(slow, LtcMode::Chase, LtcRate::Fps25, why));
    return 0;
}

int fakes_behave() {
    std::vector<float> ramp(10);
    for (std::size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>(i);
    FakeLtcSource source(ramp, 4);
    float buf[8] = {};
    CHECK(source.read({buf, 8}) == 4u);
    CHECK(buf[0] == 0.0f && buf[3] == 3.0f);
    CHECK(source.read({buf, 2}) == 2u);
    CHECK(buf[0] == 4.0f);
    CHECK(source.read({buf, 8}) == 4u);
    CHECK(buf[3] == 9.0f);
    CHECK(source.read({buf, 8}) == 0u);
    CHECK(source.remaining() == 0u);
    CHECK(source.reads() == 4u);
    source.rewind();
    CHECK(source.remaining() == 10u);

    FakeLtcSink sink(6);
    const float* storage = sink.samples().data();
    const std::uint64_t before = g_allocations.load();
    CHECK(sink.write({ramp.data(), 4}) == 4u);
    CHECK(sink.write({ramp.data() + 4, 4}) == 2u);   // full: short
    CHECK(sink.short_writes() == 1u);
    CHECK(sink.samples().size() == 6u);
    CHECK(sink.samples()[5] == 5.0f);
    sink.clear();
    CHECK(sink.write({ramp.data(), 3}) == 3u);
    CHECK(g_allocations.load() == before);
    CHECK(sink.samples().data() == storage);
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed |= frames_round_trip();
    failed |= drop_frame_vectors();
    failed |= samples_round_trip();
    failed |= chase_locks_then_plays();
    failed |= chase_window_edges();
    failed |= chase_never_locates_while_recording();
    failed |= chase_silence_budget();
    failed |= chase_rejects_and_offsets();
    failed |= chase_allocates_nothing();
    failed |= config_validation();
    failed |= fakes_behave();
    if (failed == 0) std::puts("ltc_contract: all passed");
    return failed;
}
