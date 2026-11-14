#include <cstdio>
#include <cstdlib>

#include "sonora/loops/fake_loops.h"

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);      \
            std::exit(1);                                                \
        }                                                                \
    } while (0)

using namespace sonora::loops;

int main() {
    FakeLoopSource src({
        {"a", "Funk Kit", "d/a.wav", "Drums", {"groove"}, 100.0, 8, "", 48000, 96000},
        {"b", "Warm Pad", "p/b.wav", "Synth", {"ambient"}, 120.0, 16, "Am", 48000, 1},
        {"c", "Mystery", "m/c.wav", "Drums", {}, 0.0, 0, "", 44100, 1},
    });
    CHECK(src.count() == 3);
    CHECK(src.at(1)->id == "b");
    CHECK(src.at(3) == nullptr);
    CHECK(src.search({}).size() == 3);
    LoopQuery q;
    q.text = "FUNK";
    CHECK(src.search(q).size() == 1);
    q = {};
    q.text = "ambient";
    CHECK(src.search(q).size() == 1);
    q = {};
    q.category = "Drums";
    CHECK(src.search(q).size() == 2);
    q = {};
    q.bpm = 102.0;
    q.bpm_tolerance = 0.05;
    CHECK(src.search(q).size() == 1);  // unknown bpm never matches
    q = {};
    q.key = "Am";
    CHECK(src.search(q).size() == 1);

    FakeLoopAudition aud;
    CHECK(!aud.playing());
    CHECK(aud.start(*src.at(0), 110.0) && aud.playing() && aud.current_ == "a");
    aud.stop();
    CHECK(!aud.playing());
    LoopEntry nopath;
    CHECK(!aud.start(nopath, 100.0));

    FakeLoopInserter ins;
    CHECK(ins.insert(*src.at(0), 1, 480));
    CHECK(!ins.insert(*src.at(0), 9, 0));
    CHECK(ins.calls_.size() == 1 && ins.calls_[0].position == 480);
    std::puts("ok");
    return 0;
}
