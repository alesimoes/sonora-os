// SPDX-License-Identifier: GPL-3.0-or-later
// The MTC contract (ADR-0122). The literal byte strings and drop-frame
// vectors are worked out by hand from the MTC layout (piece p carries
// nibble p of frames, seconds, minutes, hours; piece 7 also the rate), not
// computed by the code under test.
#include <cstdio>
#include <vector>

#include "fake_mtc_port.h"
#include "sonora/surfaces/mtc.h"

using namespace sonora::surfaces;

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

Timecode tc(int h, int m, int s, int f, FrameRate r) {
    Timecode t;
    t.hours = static_cast<std::uint8_t>(h);
    t.minutes = static_cast<std::uint8_t>(m);
    t.seconds = static_cast<std::uint8_t>(s);
    t.frames = static_cast<std::uint8_t>(f);
    t.rate = r;
    return t;
}

const FrameRate kRates[] = {FrameRate::Fps24, FrameRate::Fps25, FrameRate::Fps2997Drop,
                            FrameRate::Fps30};

std::vector<MtcEvent> decode(MtcDecoder& d, const Bytes& b) {
    std::vector<MtcEvent> out;
    d.feed(b.data(), b.size(), [&](const MtcEvent& e) { out.push_back(e); });
    return out;
}

Bytes cycle_bytes(const Timecode& t) {
    Bytes b(16);
    MtcEncoder::cycle(t, b.data());
    return b;
}

void test_quarter_frame_literal_bytes() {
    const Bytes want = {0xF1, 0x04, 0xF1, 0x10, 0xF1, 0x23, 0xF1, 0x30,
                        0xF1, 0x42, 0xF1, 0x50, 0xF1, 0x61, 0xF1, 0x72};
    CHECK(cycle_bytes(tc(1, 2, 3, 4, FrameRate::Fps25)) == want);
    const Bytes df = {0xF1, 0x0D, 0xF1, 0x11, 0xF1, 0x2B, 0xF1, 0x33,
                      0xF1, 0x4B, 0xF1, 0x53, 0xF1, 0x67, 0xF1, 0x75};
    CHECK(cycle_bytes(tc(23, 59, 59, 29, FrameRate::Fps2997Drop)) == df);
}

void test_full_frame_literal_bytes() {
    Bytes b(kMtcFullFrameBytes);
    MtcEncoder::full_frame(tc(1, 2, 3, 4, FrameRate::Fps25), b.data());
    CHECK((b == Bytes{0xF0, 0x7F, 0x7F, 0x01, 0x01, 0x21, 0x02, 0x03, 0x04, 0xF7}));
    MtcEncoder::full_frame(tc(23, 59, 59, 29, FrameRate::Fps2997Drop), b.data());
    CHECK((b == Bytes{0xF0, 0x7F, 0x7F, 0x01, 0x01, 0x57, 0x3B, 0x3B, 0x1D, 0xF7}));
}

// The decoded position is the carried value plus the two frames the cycle
// took; the running encoder carries a value two frames on per cycle.
void test_roundtrip_all_rates() {
    for (const FrameRate r : kRates) {
        const int last = static_cast<int>(nominal_fps(r)) - 1;
        for (const Timecode& start : {tc(0, 0, 0, 0, r), tc(1, 2, 3, 4, r), tc(10, 59, 59, last, r),
                                      tc(23, 59, 59, last, r)}) {
            MtcEncoder enc;
            enc.start(start);
            Bytes b;
            for (int i = 0; i < 8 * 5; ++i) {
                std::uint8_t q[2];
                enc.next(q);
                b.insert(b.end(), q, q + 2);
            }
            MtcDecoder dec;
            const auto ev = decode(dec, b);
            CHECK(ev.size() == 5);
            for (std::size_t i = 0; i < ev.size(); ++i) {
                CHECK(ev[i].kind == MtcEvent::Kind::QuarterFrame);
                CHECK(ev[i].position == add_frames(start, 2 * static_cast<std::int64_t>(i + 1)));
            }
            CHECK(dec.locked());

            Bytes f(kMtcFullFrameBytes);
            MtcEncoder::full_frame(start, f.data());
            const auto ff = decode(dec, f);
            CHECK(ff.size() == 1 && ff[0].kind == MtcEvent::Kind::FullFrame &&
                  ff[0].position == start);
        }
    }
    // The wrap past midnight, worked by hand: ;29 + 2 frames is 00:00:00;01
    // (minute 0 is divisible by ten, so ;00 and ;01 exist).
    MtcDecoder dec;
    const auto ev = decode(dec, cycle_bytes(tc(23, 59, 59, 29, FrameRate::Fps2997Drop)));
    CHECK(ev.size() == 1 && ev[0].position == tc(0, 0, 0, 1, FrameRate::Fps2997Drop));
}

void test_drop_frame_vectors() {
    const FrameRate df = FrameRate::Fps2997Drop;
    // Minute 1 drops ;00 and ;01: 60 s * 30 labels - 2 = 1800.
    CHECK(to_frames(tc(0, 1, 0, 2, df)) == 1800);
    CHECK(from_frames(1800, df) == tc(0, 1, 0, 2, df));
    CHECK(from_frames(1799, df) == tc(0, 0, 59, 29, df));
    CHECK(add_frames(tc(0, 0, 59, 29, df), 1) == tc(0, 1, 0, 2, df));
    CHECK(!valid(tc(0, 1, 0, 0, df)));
    CHECK(!valid(tc(0, 1, 0, 1, df)));
    // Minute 10 keeps them: 10 * 1800 - 9 * 2 = 17982.
    CHECK(to_frames(tc(0, 10, 0, 0, df)) == 17982);
    CHECK(from_frames(17982, df) == tc(0, 10, 0, 0, df));
    CHECK(from_frames(17981, df) == tc(0, 9, 59, 29, df));
    CHECK(valid(tc(0, 10, 0, 0, df)));
    CHECK(frames_per_day(df) == 2589408);
    // One drop-frame hour is 3600 s of 29.97: 107892 frames.
    CHECK(to_frames(tc(1, 0, 0, 0, df)) == 107892);
}

void test_frames_and_samples_roundtrip() {
    for (const FrameRate r : kRates) {
        for (std::int64_t n = 0; n < frames_per_day(r); n += 997) {
            const Timecode t = from_frames(n, r);
            CHECK(valid(t));
            CHECK(to_frames(t) == n);
            for (const std::uint32_t sr : {44100u, 48000u, 96000u}) {
                CHECK(from_samples(to_samples(t, sr), r, sr) == t);
            }
        }
    }
    CHECK(to_samples(tc(0, 0, 1, 0, FrameRate::Fps25), 48000) == 48000);
    CHECK(to_samples(tc(1, 0, 0, 0, FrameRate::Fps24), 48000) == 3600ull * 48000);
    // 1 frame of 29.97 at 48 kHz is 1601.6 samples: the first sample in it.
    CHECK(frames_to_samples(1, FrameRate::Fps2997Drop, 48000) == 1602);
    // The drop-frame label 01:00:00;00 is 107892 frames of 1001/30000 s,
    // 3.6 ms short of a real hour: 107892 * 1601.6 = 172799827.2, ceil.
    CHECK(to_samples(tc(1, 0, 0, 0, FrameRate::Fps2997Drop), 48000) == 172799828ull);
}

void test_split_chunks() {
    const Timecode start = tc(1, 2, 3, 4, FrameRate::Fps25);
    Bytes b = cycle_bytes(start);
    Bytes f(kMtcFullFrameBytes);
    MtcEncoder::full_frame(tc(5, 6, 7, 8, FrameRate::Fps30), f.data());
    b.insert(b.end(), f.begin(), f.end());
    const Bytes again = cycle_bytes(tc(5, 6, 7, 8, FrameRate::Fps30));
    b.insert(b.end(), again.begin(), again.end());

    for (const std::size_t chunk : {std::size_t{1}, std::size_t{3}, std::size_t{7}}) {
        FakeMtcPort port;
        port.set_read_chunk(chunk);
        port.feed(b.data(), b.size());
        MtcDecoder dec;
        std::vector<MtcEvent> ev;
        const std::size_t read = dec.poll(port, [&](const MtcEvent& e) { ev.push_back(e); });
        CHECK(read == b.size());
        CHECK(port.reads > b.size() / chunk);
        CHECK(ev.size() == 3);
        if (ev.size() != 3) continue;
        CHECK(ev[0].kind == MtcEvent::Kind::QuarterFrame &&
              ev[0].position == tc(1, 2, 3, 6, FrameRate::Fps25));
        CHECK(ev[1].kind == MtcEvent::Kind::FullFrame &&
              ev[1].position == tc(5, 6, 7, 8, FrameRate::Fps30));
        CHECK(ev[2].position == tc(5, 6, 7, 10, FrameRate::Fps30));
    }

    // Real-time bytes inside a cycle and inside the SysEx break nothing.
    Bytes noisy = cycle_bytes(start);
    noisy.insert(noisy.begin() + 5, 0xF8);
    noisy.insert(noisy.begin() + 9, 0xFE);
    noisy.insert(noisy.end(), {0xF0, 0x7F, 0xF8, 0x7F, 0x01, 0x01, 0x21, 0x02, 0x03, 0x04, 0xF7});
    MtcDecoder dec;
    const auto ev = decode(dec, noisy);
    CHECK(ev.size() == 2);
    CHECK(ev.size() == 2 && ev[1].position == start);
}

void test_gap_resets_lock() {
    const Timecode start = tc(0, 0, 10, 0, FrameRate::Fps24);
    MtcDecoder dec;
    CHECK(decode(dec, cycle_bytes(start)).size() == 1);
    CHECK(dec.locked());

    // Piece 2 of the next cycle goes missing.
    Bytes gap = cycle_bytes(add_frames(start, 2));
    gap.erase(gap.begin() + 4, gap.begin() + 6);
    CHECK(decode(dec, gap).empty());
    CHECK(!dec.locked());

    // A cycle entered half way is not enough either.
    const Bytes half = cycle_bytes(add_frames(start, 4));
    CHECK(decode(dec, Bytes(half.begin() + 8, half.end())).empty());
    CHECK(!dec.locked());

    // A whole 0..7 locks again.
    const auto ev = decode(dec, cycle_bytes(add_frames(start, 6)));
    CHECK(ev.size() == 1 && ev[0].position == add_frames(start, 8));
    CHECK(dec.locked());

    // A cycle run backwards (reverse play) never locks.
    MtcDecoder rev;
    const Bytes fwd = cycle_bytes(start);
    Bytes back;
    for (int p = 7; p >= 0; --p) back.insert(back.end(), {fwd[2 * p], fwd[2 * p + 1]});
    CHECK(decode(rev, back).empty());
    CHECK(!rev.locked());

    // A value out of range drops the lock instead of reporting it.
    Bytes bad = cycle_bytes(tc(0, 0, 0, 0, FrameRate::Fps25));
    bad[1] = 0x0F;  // frames low nibble 15, high nibble 1 below: frame 31
    bad[3] = 0x11;
    CHECK(decode(dec, bad).empty());
    CHECK(!dec.locked());
}

void test_chase_rule() {
    const std::uint32_t sr = 48000;
    const FrameRate r = FrameRate::Fps25;
    ChaseRule chase(sr);
    ChaseSnapshot engine;  // stopped at 0

    // Far away: locate there first.
    const Timecode at = tc(1, 0, 0, 0, r);
    CHECK(chase.on_position(at, engine) == ChaseAction::Locate);
    CHECK(chase.target() == 3600ull * sr);

    // Located, master moving two frames on: roll.
    engine.playhead = chase.target();
    CHECK(chase.on_position(add_frames(at, 2), engine) == ChaseAction::Play);

    // Rolling inside the 2-frame window (1 frame = 1920 samples): nothing.
    engine.rolling = true;
    engine.playhead = to_samples(add_frames(at, 4), sr) + 3000;
    CHECK(chase.on_position(add_frames(at, 4), engine) == ChaseAction::None);
    // Drifted past it: locate while rolling.
    engine.playhead = to_samples(add_frames(at, 6), sr) + 2 * 1920 + 1;
    CHECK(chase.on_position(add_frames(at, 6), engine) == ChaseAction::Locate);

    // Silence counted in frame periods: stop after 3, once.
    CHECK(chase.tick(2, engine) == ChaseAction::None);
    CHECK(chase.tick(1, engine) == ChaseAction::Stop);
    CHECK(chase.tick(5, engine) == ChaseAction::None);

    // The same position twice is a parked master: no roll.
    ChaseRule parked(sr);
    ChaseSnapshot stopped;
    stopped.playhead = to_samples(at, sr);
    CHECK(parked.on_position(at, stopped) == ChaseAction::None);
    CHECK(parked.on_position(at, stopped) == ChaseAction::None);
    // Silence while the engine is stopped sends nothing.
    CHECK(parked.tick(10, stopped) == ChaseAction::None);
}
}  // namespace

int main() {
    test_quarter_frame_literal_bytes();
    test_full_frame_literal_bytes();
    test_roundtrip_all_rates();
    test_drop_frame_vectors();
    test_frames_and_samples_roundtrip();
    test_split_chunks();
    test_gap_resets_lock();
    test_chase_rule();
    if (failures == 0) std::printf("surface_mtc_contract: all passed\n");
    return failures == 0 ? 0 : 1;
}
