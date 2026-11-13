#include "helpers.h"
using namespace stepseq_test;

int main() {
    StepPatternPlayer pl;
    Pattern p = base_pattern();
    p.set_step(0, 0, on_step());
    p.set_step(1, 0, on_step());
    CHECK(render(pl, p, 6000, 6000).size() == 2);

    Row r = p.row(0);
    r.mute = true;
    p.set_row(0, r);
    auto h = render(pl, p, 6000, 6000);
    CHECK(h.size() == 1 && h[0].row == 1);

    r.mute = false;
    r.solo = true;
    p.set_row(0, r);
    h = render(pl, p, 6000, 6000);
    CHECK(h.size() == 1 && h[0].row == 0);
    r.mute = true; // a muted soloed row stays silent
    p.set_row(0, r);
    CHECK(render(pl, p, 6000, 6000).empty());

    p = base_pattern();
    p.set_row(1, Row{}); // no source
    p.set_step(1, 0, on_step());
    CHECK(render(pl, p, 6000, 6000).empty());

    // Length 4 loops; step_index stays absolute across the wrap and blocks
    // may straddle it.
    p = base_pattern();
    p.set_length(4);
    p.set_step(0, 0, on_step());
    h = render(pl, p, 6000 * 12, 4000);
    CHECK(h.size() == 3);
    CHECK(h[0].frame == 0 && h[1].frame == 24000 && h[2].frame == 48000);
    CHECK(h[1].step == 4 && h[2].step == 8);
    return 0;
}
