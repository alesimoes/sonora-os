#include "sonora/stepseq/step_player.h"

#include <cmath>

#include "sonora/stepseq/fake_player.h"
#include "step_timing.h"

namespace sonora::stepseq {
namespace {

// Swing and nudge only reorder neighbours, so the shift is a few slots.
void insert_sorted(EventBlock& out, const TriggerEvent& e) noexcept {
    if (out.count >= kMaxEventsPerBlock) {
        ++out.dropped;
        return;
    }
    int i = out.count++;
    while (i > 0 && out.events[i - 1].frame_offset > e.frame_offset) {
        out.events[i] = out.events[i - 1];
        --i;
    }
    out.events[i] = e;
}

TriggerEvent make_event(const Pattern& p, const StepTiming& t, int r, const Step& st,
                        std::int64_t n, std::int64_t at, std::int64_t block_start) noexcept {
    const Row& row = p.row(r);
    TriggerEvent e;
    e.frame_offset = static_cast<std::int32_t>(at - block_start);
    const long long gate = std::llround(st.gate * t.frames_per_step);
    e.gate_frames = static_cast<std::int32_t>(gate < 1 ? 1 : gate);
    e.step_index = n;
    e.source = row.source;
    e.gain_db = row.gain_db;
    e.pan = row.pan;
    e.row = static_cast<std::uint8_t>(r);
    e.velocity = st.velocity;
    e.pitch_semitones = st.pitch_semitones;
    return e;
}

} // namespace

void StepPatternPlayer::render_events(const Pattern& pattern, std::int64_t block_start_frame,
                                      std::int32_t frames, const TempoSnapshot& tempo,
                                      EventBlock& out) noexcept {
    out.clear();
    StepTiming t;
    if (frames <= 0 || !make_step_timing(pattern, tempo, t)) return;

    const bool any_solo = pattern.any_solo();
    const int max_nudge = pattern.max_nudge_ticks();
    const std::int64_t block_end = block_start_frame + frames;
    std::int64_t first = 0, last = 0;
    scan_range(t, tempo.origin_frame, block_start_frame, block_end, first, last);

    for (std::int64_t n = first; n <= last; ++n) {
        const int s = static_cast<int>(n % pattern.length());
        for (int r = 0; r < pattern.row_count(); ++r) {
            const Step& st = pattern.step(r, s);
            if (!st.on || st.velocity == 0 || !pattern.row_audible(r, any_solo)) continue;
            const int nudge = st.nudge_ticks < -max_nudge ? -max_nudge
                              : (st.nudge_ticks > max_nudge ? max_nudge : st.nudge_ticks);
            const std::int64_t at = trigger_frame(t, tempo.origin_frame, n, nudge);
            if (at < block_start_frame || at >= block_end) continue;
            if (!FakePatternPlayer::fires(seed_, n, r, st.probability)) continue;
            insert_sorted(out, make_event(pattern, t, r, st, n, at, block_start_frame));
        }
    }
}

} // namespace sonora::stepseq
