#include "helpers.h"
using namespace stepseq_test;

int main() {
    Pattern p = base_pattern();
    for (int s = 0; s < 16; ++s) p.set_step(0, s, on_step());
    StepPatternPlayer pl;
    p.set_swing_percent(50);
    auto h = render(pl, p, 6000 * 16, 6000);
    CHECK(h.size() == 16 && h[1].frame == 6000);

    // 75 % delays every odd step by half a step (3000 frames).
    p.set_swing_percent(75);
    h = render(pl, p, 6000 * 16, 6000);
    CHECK(h.size() == 16);
    CHECK(h[0].frame == 0 && h[1].frame == 9000 && h[2].frame == 12000 && h[3].frame == 21000);

    // A cut between a step and its swung hit changes nothing.
    auto g = render(pl, p, 6000 * 16, 8000);
    CHECK(g.size() == 16);
    for (size_t i = 0; i < g.size(); ++i) CHECK(g[i].frame == h[i].frame);
    for (size_t i = 1; i < h.size(); ++i) CHECK(h[i].frame >= h[i - 1].frame);
    return 0;
}
