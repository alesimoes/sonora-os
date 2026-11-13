#include <cstdio>
#include <cstdlib>

#include "sonora/loops/fake_loops.h"
#include "sonora/loops/loop_scan.h"

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);      \
            std::exit(1);                                                \
        }                                                                \
    } while (0)

using namespace sonora::loops;

int main() {
    std::vector<LoopEntry> all = {
        {"a", "Funk Kit", "d/a.wav", "Drums", {"groove"}, 100.0, 8, "", 48000, 96000},
        {"b", "Warm Pad", "p/b.wav", "Synth", {"ambient"}, 120.0, 16, "Am", 48000, 1},
        {"c", "Mystery", "m/c.wav", "Drums", {}, 0.0, 0, "", 44100, 1},
    };
    CHECK(filter(all, {}).size() == 3);
    LoopQuery q;
    q.text = "GROOVE";
    CHECK(filter(all, q).size() == 1);
    q = {};
    q.category = "Drums";
    CHECK(filter(all, q).size() == 2);
    q = {};
    q.bpm = 102.0;
    q.bpm_tolerance = 0.05;
    CHECK(filter(all, q).size() == 1);  // unknown bpm never matches
    q.bpm_tolerance = 0.0;
    CHECK(filter(all, q).empty());
    q = {};
    q.key = "Am";
    CHECK(filter(all, q).size() == 1);

    // Same answers as the contract fake.
    FakeLoopSource fake(all);
    q = {};
    q.text = "pad";
    CHECK(filter(all, q).size() == fake.search(q).size());
    std::printf("loops_filter ok\n");
    return 0;
}
