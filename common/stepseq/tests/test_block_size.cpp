// Probability draws must not depend on how the timeline is cut into blocks
// (ADR-0169): the same seed gives the same hits for every buffer size the
// engine may run at, with swing, nudge and a non-zero origin in play, and when
// the blocks come through the exchange and the feed.

#include <tuple>

#include "helpers.h"
#include "sonora/stepseq/pattern_feed.h"

using namespace stepseq_test;

namespace {

Pattern busy_pattern() {
    Pattern p = base_pattern();
    p.set_length(32);
    p.set_swing_percent(62);
    for (int i = 0; i < 32; ++i) {
        Step s = on_step();
        s.probability = static_cast<std::uint8_t>(10 + (i * 37) % 81);
        s.nudge_ticks = static_cast<std::int16_t>((i % 5 - 2) * 40);
        p.set_step(0, i, s);
        s.probability = 50;
        s.nudge_ticks = static_cast<std::int16_t>(-s.nudge_ticks);
        p.set_step(1, i, s);
    }
    return p;
}

// The last block may run past `end`; hits there are not in the reference.
std::vector<Hit> before(std::vector<Hit> h, std::int64_t end) {
    while (!h.empty() && h.back().frame >= end) h.pop_back();
    return h;
}

bool same(const std::vector<Hit>& a, const std::vector<Hit>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tie(a[i].frame, a[i].step, a[i].row) != std::tie(b[i].frame, b[i].step, b[i].row))
            return false;
    return true;
}

std::vector<Hit> render_through_feed(const Pattern& p, std::int64_t total, int block,
                                     const TempoSnapshot& t) {
    auto x = std::make_unique<PatternExchange>();
    CHECK(x->publish(p));
    StepPatternPlayer pl;
    pl.reset(42);
    PatternFeed feed(*x, pl);
    std::vector<Hit> hits;
    auto eb = std::make_unique<EventBlock>();
    for (std::int64_t at = 0; at < total; at += block) {
        feed.render_block(at, block, t, *eb);
        for (int i = 0; i < eb->count; ++i)
            hits.push_back({at + eb->events[i].frame_offset, eb->events[i].step_index,
                            eb->events[i].row});
    }
    return hits;
}

} // namespace

int main() {
    TempoSnapshot t = tempo();
    t.origin_frame = 1777; // the pattern starts off any block boundary
    t.bpm = 133.0;
    const Pattern p = busy_pattern();
    const std::int64_t total = 48000 * 30;

    StepPatternPlayer ref;
    ref.reset(42);
    const auto whole = render(ref, p, total, 48000, t);
    CHECK(whole.size() > 100); // the draws keep some hits and drop others
    CHECK(whole.size() < 532); // 2 rows x 266 steps in 30 s at 133 bpm

    for (int block : {1, 16, 64, 127, 128, 256, 441, 1024, 4096, 6000}) {
        StepPatternPlayer pl;
        pl.reset(42);
        // Block 1 visits every frame; a shorter stretch keeps it quick.
        const std::int64_t span = block == 1 ? 48000 * 3 : total;
        const auto small = before(render(pl, p, span, block, t), span);
        const auto ref_span = before(render(ref, p, span, 48000, t), span);
        CHECK(same(small, ref_span));
        CHECK(same(before(render_through_feed(p, span, block, t), span), ref_span));
    }
    std::printf("stepseq block size: ok\n");
    return 0;
}
