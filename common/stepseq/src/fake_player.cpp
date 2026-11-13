#include "sonora/stepseq/fake_player.h"

#include <cmath>

namespace sonora::stepseq {
namespace {

std::uint64_t splitmix64(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Keeps `out` sorted by frame as events arrive; swing and nudge only reorder
// neighbours, so the shift is a few slots at most.
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

} // namespace

bool FakePatternPlayer::fires(std::uint64_t seed, std::int64_t step_index, int row,
                              int probability) noexcept {
    if (probability >= kMaxProbability) return true;
    if (probability <= 0) return false;
    const std::uint64_t key = seed ^ splitmix64(static_cast<std::uint64_t>(step_index))
                              ^ (static_cast<std::uint64_t>(row) << 56);
    return static_cast<int>(splitmix64(key) % 100u) < probability;
}

void FakePatternPlayer::render_events(const Pattern& pattern, std::int64_t block_start_frame,
                                      std::int32_t frames, const TempoSnapshot& tempo,
                                      EventBlock& out) noexcept {
    out.clear();
    if (frames <= 0 || !(tempo.sample_rate > 0.0) || !(tempo.bpm > 0.0)) return;

    const double frames_per_beat = tempo.sample_rate * 60.0 / tempo.bpm;
    const double frames_per_step = frames_per_beat / pattern.steps_per_beat();
    const double swing_frames =
        (pattern.swing_percent() / 100.0 - 0.5) * 2.0 * frames_per_step;
    const double frames_per_tick = frames_per_beat / kTicksPerBeat;
    const int length = pattern.length();
    const bool any_solo = pattern.any_solo();
    // Pattern already keeps nudges inside half a step; clamping again here
    // keeps the scan margin below valid whatever path built the snapshot.
    const int max_nudge = pattern.max_nudge_ticks();

    const std::int64_t block_end = block_start_frame + frames;
    // Swing plus nudge move a trigger at most one step late or half a step
    // early (Pattern clamps nudge), so a margin of two steps either side
    // catches every step whose trigger can land in this block.
    const auto first = static_cast<std::int64_t>(
        std::floor((block_start_frame - tempo.origin_frame) / frames_per_step)) - 2;
    const auto last = static_cast<std::int64_t>(
        std::floor((block_end - tempo.origin_frame) / frames_per_step)) + 2;

    for (std::int64_t n = first < 0 ? 0 : first; n <= last; ++n) {
        const int s = static_cast<int>(n % length);
        const double base = static_cast<double>(n) * frames_per_step + (s % 2 ? swing_frames : 0.0);
        for (int r = 0; r < pattern.row_count(); ++r) {
            const Step& st = pattern.step(r, s);
            if (!st.on || st.velocity == 0 || !pattern.row_audible(r, any_solo)) continue;
            const int nudge = st.nudge_ticks < -max_nudge ? -max_nudge
                              : (st.nudge_ticks > max_nudge ? max_nudge : st.nudge_ticks);
            const std::int64_t at = tempo.origin_frame
                + std::llround(base + nudge * frames_per_tick);
            if (at < block_start_frame || at >= block_end) continue;
            if (!fires(seed_, n, r, st.probability)) continue;

            const Row& row = pattern.row(r);
            TriggerEvent e;
            e.frame_offset = static_cast<std::int32_t>(at - block_start_frame);
            const long long gate = std::llround(st.gate * frames_per_step);
            e.gate_frames = static_cast<std::int32_t>(gate < 1 ? 1 : gate);
            e.step_index = n;
            e.source = row.source;
            e.gain_db = row.gain_db;
            e.pan = row.pan;
            e.row = static_cast<std::uint8_t>(r);
            e.velocity = st.velocity;
            e.pitch_semitones = st.pitch_semitones;
            insert_sorted(out, e);
        }
    }
}

} // namespace sonora::stepseq
