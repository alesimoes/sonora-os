// SPDX-License-Identifier: GPL-3.0-or-later
// WarpMap validation, output length, inverse lookup, and the fake stretcher.
#include <cmath>
#include <cstdio>

#include "sonora/stretch/stretcher.h"

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                            \
        }                                                                        \
    } while (0)

using namespace sonora::stretch;

int main() {
    std::string why;
    WarpMap m;
    CHECK(!m.set(0, {}, why));
    CHECK(!m.set(100, {{0, 5}}, why));          // source must be > 0
    CHECK(!m.set(100, {{5, 0}}, why));          // target must be > 0
    CHECK(!m.set(100, {{10, 10}, {10, 20}}, why));  // source not increasing
    CHECK(!m.set(100, {{10, 10}, {20, 10}}, why));  // target not increasing
    CHECK(!m.set(100, {{100, 100}}, why));      // source outside the region
    CHECK(m.source_length() == 0);              // failed sets leave it unchanged
    CHECK(!why.empty());

    CHECK(m.set(100, {}, why));
    CHECK(m.output_length() == 100);
    CHECK(m.set(100, {{40, 80}}, why));
    CHECK(m.output_length() == 80 + 60);  // the tail keeps its speed
    CHECK(m.source_at(0) == 0.0);
    CHECK(m.source_at(40) == 20.0);       // first segment at half speed
    CHECK(m.source_at(80) == 40.0);
    CHECK(m.source_at(110) == 70.0);      // tail slope 1
    CHECK(m.source_at(1e9) == 100.0);     // clamped
    CHECK(m.source_at(-5) == 0.0);

    // The fake: output length equals the target span, content follows the map.
    Planar in(2, std::vector<float>(100));
    for (int i = 0; i < 100; ++i) in[0][i] = in[1][i] = static_cast<float>(i);
    LinearResampleStretcher fake;
    Planar out;
    CHECK(fake.process(in, m, Algorithm::Rhythmic, out, why));
    CHECK(out.size() == 2 && out[0].size() == 140);
    CHECK(std::fabs(out[0][40] - 20.0f) < 1e-4f);
    CHECK(std::fabs(out[1][110] - 70.0f) < 1e-4f);

    WarpMap id;
    CHECK(id.set(100, {}, why));
    CHECK(fake.process(in, id, Algorithm::Varispeed, out, why));
    CHECK(out[0] == in[0]);  // identity map is a copy

    Planar bad(2);
    bad[0].assign(100, 0.f);
    bad[1].assign(99, 0.f);
    CHECK(!fake.process(bad, m, Algorithm::Polyphonic, out, why));
    CHECK(!fake.process(Planar{}, m, Algorithm::Polyphonic, out, why));
    Planar shorter(1, std::vector<float>(50));
    CHECK(!fake.process(shorter, m, Algorithm::Polyphonic, out, why));
    return 0;
}
