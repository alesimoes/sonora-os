#pragma once
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "sonora/stepseq/step_player.h"

#define CHECK(c) \
    do { \
        if (!(c)) { \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
            std::exit(1); \
        } \
    } while (0)

namespace stepseq_test {
using namespace sonora::stepseq;

// Defaults: 48 kHz, 120 bpm, 4 steps/beat -> 6000 frames per step.
inline TempoSnapshot tempo() { return TempoSnapshot{}; }

inline Pattern base_pattern() {
    Pattern p;
    p.set_length(16);
    p.set_row_count(2);
    Row r;
    r.source = 7;
    p.set_row(0, r);
    r.source = 8;
    p.set_row(1, r);
    return p;
}

inline Step on_step() {
    Step s;
    s.on = true;
    return s;
}

struct Hit {
    std::int64_t frame;
    std::int64_t step;
    int row;
};

// Renders [0, total) in blocks of `block` and returns absolute hits.
inline std::vector<Hit> render(PatternPlayer& pl, const Pattern& p, std::int64_t total, int block,
                               const TempoSnapshot& t = tempo()) {
    std::vector<Hit> hits;
    auto eb = std::make_unique<EventBlock>();
    for (std::int64_t at = 0; at < total; at += block) {
        pl.render_events(p, at, block, t, *eb);
        for (int i = 0; i < eb->count; ++i)
            hits.push_back({at + eb->events[i].frame_offset, eb->events[i].step_index,
                            eb->events[i].row});
    }
    return hits;
}

} // namespace stepseq_test
