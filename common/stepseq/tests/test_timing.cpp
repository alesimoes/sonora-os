#include "helpers.h"
using namespace stepseq_test;

int main() {
    Pattern p = base_pattern();
    Step s = on_step();
    p.set_step(0, 0, s);
    p.set_step(0, 5, s);
    s.gate = 0.25f;
    p.set_step(0, 9, s);
    StepPatternPlayer pl;
    auto hits = render(pl, p, 6000 * 16, 6000);
    CHECK(hits.size() == 3);
    CHECK(hits[0].frame == 0 && hits[1].frame == 30000 && hits[2].frame == 54000);

    // A block edge exactly on a trigger puts it in the later block.
    auto eb = std::make_unique<EventBlock>();
    pl.render_events(p, 29999, 1, tempo(), *eb);
    CHECK(eb->count == 0);
    pl.render_events(p, 30000, 1, tempo(), *eb);
    CHECK(eb->count == 1 && eb->events[0].frame_offset == 0);
    CHECK(eb->events[0].gate_frames == 3000);

    // Odd block sizes give the same frames as aligned ones.
    auto a = render(pl, p, 6000 * 16, 997);
    CHECK(a.size() == hits.size());
    for (size_t i = 0; i < a.size(); ++i) CHECK(a[i].frame == hits[i].frame);

    // 120 ticks of 960 per beat = 3000 frames at 120 bpm / 48 kHz.
    Step n = on_step();
    n.nudge_ticks = 120;
    p.set_step(0, 0, n);
    CHECK(render(pl, p, 6000, 6000)[0].frame == 3000);

    TempoSnapshot t = tempo();
    t.origin_frame = 100;
    CHECK(render(pl, p, 7000, 7000, t)[0].frame == 3100);
    return 0;
}
