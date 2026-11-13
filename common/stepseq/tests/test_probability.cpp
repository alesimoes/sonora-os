#include "helpers.h"
using namespace stepseq_test;

namespace {
Pattern prob_pattern(int pct) {
    Pattern p = base_pattern();
    p.set_length(64);
    Step s = on_step();
    s.probability = static_cast<std::uint8_t>(pct);
    for (int i = 0; i < 64; ++i) p.set_step(0, i, s);
    return p;
}
} // namespace

int main() {
    const std::int64_t total = 6000 * 64 * 4;
    StepPatternPlayer a, b;
    a.reset(42);
    b.reset(42);
    Pattern p = prob_pattern(50);
    auto ha = render(a, p, total, 6000);
    auto hb = render(b, p, total, 1234); // block size must not change the draws
    CHECK(ha.size() == hb.size());
    for (size_t i = 0; i < ha.size(); ++i) CHECK(ha[i].frame == hb[i].frame);
    CHECK(ha.size() > 90 && ha.size() < 170); // about half of 256

    b.reset(43);
    auto hc = render(b, p, total, 6000);
    bool differs = hc.size() != ha.size();
    for (size_t i = 0; !differs && i < ha.size(); ++i) differs = ha[i].frame != hc[i].frame;
    CHECK(differs);

    CHECK(render(a, prob_pattern(100), total, 6000).size() == 256);
    CHECK(render(a, prob_pattern(0), total, 6000).empty());
    return 0;
}
