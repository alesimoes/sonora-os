// SPDX-License-Identifier: GPL-3.0-or-later
//
// A loop take's passes from its wraps (ADR-0016 §1, §5): pass k ends at
// wrap k and is B - A long; the pass after the last wrap is cut short by
// the stop. A wrap the list does not hold -- lost to a full boundary ring,
// or past a wrap list that stops early or is missing -- is derived from the
// loop length: B - TimeReference + L for the first, B - A after the one
// before. Recovery reads all of it from the take's file and wrap list.
#include "sonora/loop_take.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "sonora/session.h"
#include "sonora/wav_writer.h"
#include "sonora/wrap_sidecar.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

namespace fs = std::filesystem;
using sonora::LoopPass;
using sonora::WrapSidecar;
constexpr uint64_t kLost = WrapSidecar::kLostWrap;

// A = 1000, B = 1300, the take from 900 (pre-roll 100), L = 0: wraps at
// 400, 700, 1000, ...
constexpr uint64_t kA = 1000, kB = 1300, kStart = 900;

int test_known_wraps_are_kept() {
    bool derived = true;
    const std::vector<uint64_t> w =
        sonora::complete_wraps({400, 700, 1000}, 3, 1100, kA, kB, kStart, 0, derived);
    CHECK((w == std::vector<uint64_t>{400, 700, 1000}));
    CHECK(!derived);
    return 0;
}

// A lost wrap is the one before it plus B - A; a lost first one is the
// TimeReference's B, plus L; and the ones the list never got, up to the
// audio thread's count, follow the last.
int test_lost_wraps_are_derived() {
    bool derived = false;
    std::vector<uint64_t> w =
        sonora::complete_wraps({400, kLost, 1000}, 3, 1100, kA, kB, kStart, 0, derived);
    CHECK((w == std::vector<uint64_t>{400, 700, 1000}));
    CHECK(derived);
    derived = false;
    w = sonora::complete_wraps({kLost, 707}, 2, 800, kA, kB, kStart, 7, derived);
    CHECK((w == std::vector<uint64_t>{407, 707}));
    CHECK(derived);
    derived = false;
    w = sonora::complete_wraps({400}, 3, 1100, kA, kB, kStart, 0, derived);
    CHECK((w == std::vector<uint64_t>{400, 700, 1000}));
    CHECK(derived);
    // No list at all, as after a full ring on every wrap.
    w = sonora::complete_wraps({}, 2, 800, kA, kB, kStart, 0, derived);
    CHECK((w == std::vector<uint64_t>{400, 700}));
    return 0;
}

// Recovery knows no count: the wraps go on while the file does.
int test_until_the_file_ends() {
    bool derived = false;
    std::vector<uint64_t> w = sonora::complete_wraps({400}, sonora::kWrapsUntilEnd, 1000, kA, kB,
                                                     kStart, 0, derived);
    CHECK((w == std::vector<uint64_t>{400, 700, 1000}));
    CHECK(derived);
    derived = false;
    w = sonora::complete_wraps({400, 700, 1000}, sonora::kWrapsUntilEnd, 999, kA, kB, kStart, 0,
                               derived);
    CHECK((w == std::vector<uint64_t>{400, 700, 1000}));
    CHECK(!derived);
    return 0;
}

int test_passes() {
    // Two whole passes and a third cut short at 1100.
    std::vector<LoopPass> p = sonora::loop_passes({400, 700}, 1100, kA, kB, kStart, 0);
    CHECK(p.size() == 3);
    CHECK(p[0].offset == 100 && p[0].length == 300 && p[0].complete);
    CHECK(p[1].offset == 400 && p[1].length == 300 && p[1].complete);
    CHECK(p[2].offset == 700 && p[2].length == 300 && !p[2].complete);
    // Stopped exactly on a wrap: no cut-short pass.
    p = sonora::loop_passes({400, 700}, 700, kA, kB, kStart, 0);
    CHECK(p.size() == 2 && p[1].complete);
    // Stopped before the first wrap: one pass, cut short, from A.
    p = sonora::loop_passes({}, 250, kA, kB, kStart, 0);
    CHECK(p.size() == 1);
    CHECK(p[0].offset == 100 && p[0].length == 150 && !p[0].complete);
    // Stopped in the pre-roll: no pass.
    CHECK(sonora::loop_passes({}, 90, kA, kB, kStart, 0).empty());
    // With latency the wraps, and so the passes, are L later in the file.
    p = sonora::loop_passes({437}, 600, kA, kB, kStart, 37);
    CHECK(p.size() == 2);
    CHECK(p[0].offset == 137 && p[0].complete);
    CHECK(p[1].offset == 437 && p[1].length == 163 && !p[1].complete);
    // A wrap past what reached the file ends no pass in it.
    p = sonora::loop_passes({400, 705}, 700, kA, kB, kStart, 0);
    CHECK(p.size() == 2 && !p[1].complete);
    return 0;
}

std::string write_take(const fs::path& dir, const char* name, uint64_t frames, bool loop) {
    const std::string path = (dir / name).string();
    fs::remove(path);
    fs::remove(WrapSidecar::path_for(path));
    sonora::WavWriter w;
    sonora::LoopRange l;
    l.from = kA;
    l.to = kB;
    w.set_description(sonora::take_description(4, 9, "Demo", nullptr, loop ? &l : nullptr));
    if (!w.open(path, 48000, 1, sonora::WavWriter::Format::Float32, kStart)) return {};
    std::vector<float> s(frames, 0.0f);
    w.write(s.data(), frames);
    w.close();
    return path;
}

// Recovery reads the wraps from the list, and derives the rest from the
// Description's loop= and the TimeReference when the list stops early or is
// missing, saying so.
int test_recovery_reads_the_take() {
    const fs::path dir = fs::temp_directory_path() / "sonora_test_loop_passes";
    fs::create_directories(dir);
    const std::string take = write_take(dir, "Audio 1_04.wav", 1000, true);
    CHECK(!take.empty());
    {
        WrapSidecar side;
        CHECK(side.open(WrapSidecar::path_for(take), kA, kB, 0));
        CHECK(side.append(400));
        CHECK(side.append(700));
        CHECK(side.append(1000));
        CHECK(side.close());
    }
    sonora::RecoveredLoop r;
    CHECK(sonora::recover_loop_wraps(take, r));
    CHECK(r.from == kA && r.to == kB && r.time_reference == kStart && r.frames == 1000);
    CHECK((r.wraps == std::vector<uint64_t>{400, 700, 1000}));
    CHECK(!r.by_length);

    // Killed before the last wrap reached the list.
    fs::resize_file(WrapSidecar::path_for(take), WrapSidecar::kHeaderBytes + 8);
    CHECK(sonora::recover_loop_wraps(take, r));
    CHECK((r.wraps == std::vector<uint64_t>{400, 700, 1000}));
    CHECK(r.by_length);

    fs::remove(WrapSidecar::path_for(take));
    CHECK(sonora::recover_loop_wraps(take, r));
    CHECK((r.wraps == std::vector<uint64_t>{400, 700, 1000}));
    CHECK(r.by_length);

    // Code review of PR #117, N4: an orphan's L comes from its list. Killed
    // before its first wrap reached the list, every wrap is derived from
    // the header's L, not from one the caller supplies.
    const std::string late = write_take(dir, "Audio 1_06.wav", 1100, true);
    CHECK(!late.empty());
    {
        WrapSidecar side;
        CHECK(side.open(WrapSidecar::path_for(late), kA, kB, 50));
        CHECK(side.close());
    }
    CHECK(sonora::recover_loop_wraps(late, r));
    CHECK(r.latency == 50);
    CHECK((r.wraps == std::vector<uint64_t>{450, 750, 1050}));
    CHECK(r.by_length);

    // Not a loop take: nothing to place by pass.
    const std::string plain = write_take(dir, "Audio 1_05.wav", 1000, false);
    CHECK(!sonora::recover_loop_wraps(plain, r));
    std::error_code ec;
    fs::remove_all(dir, ec);
    return 0;
}

}  // namespace

int main() {
    if (test_known_wraps_are_kept()) return 1;
    if (test_lost_wraps_are_derived()) return 1;
    if (test_until_the_file_ends()) return 1;
    if (test_passes()) return 1;
    if (test_recovery_reads_the_take()) return 1;
    std::printf("loop_passes: ok\n");
    return 0;
}
