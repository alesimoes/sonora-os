#include <cmath>
#include <cstdio>
#include <vector>

#include "sonora/stems/separators.h"
#include "sonora/stems/stem_separator.h"

using namespace sonora::stems;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

struct Stop : ProgressSink {
    int calls = 0;
    bool report(double) override { return ++calls < 2; }
};

int main() {
    std::vector<float> l(100), r(100);
    for (int i = 0; i < 100; ++i) { l[i] = std::sin(i * 0.1f); r[i] = 0.5f * i / 100; }
    const float* in[2] = {l.data(), r.data()};
    StemRequest req{in, 2, 100, 48000};
    FakeSeparator f;
    StemResult res;
    std::string err;
    CHECK(f.separate(req, res, nullptr, &err));
    CHECK(res.stems.size() == kStemKindCount);
    CHECK(!f.supports(StemKind::Harmonic));
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 100; ++i) {
            float s = 0;
            for (auto& v : res.stems) if (!v.empty()) s += v[c * 100 + i];
            CHECK(std::fabs(s - in[c][i]) < 1e-6f);
        }
    Stop stop;
    StemResult r2;
    CHECK(!f.separate(req, r2, &stop, &err));
    CHECK(err == "cancelled");
    CHECK(!f.separate(StemRequest{}, r2, nullptr, &err));
    CHECK(default_separators().empty());
    return fails ? 1 : 0;
}
