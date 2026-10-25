// SPDX-License-Identifier: GPL-3.0-or-later
#include "time_format.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sonora::ui {

namespace {

struct FrameRate {
    int64_t num = 25, den = 1;  // video frames per second as a ratio
    int base = 25;              // frames per timecode second
    bool drop = false;
};

FrameRate frame_rate(model::Fps fps) {
    switch (fps) {
        case model::Fps::F23_976: return {24000, 1001, 24, false};
        case model::Fps::F24: return {24, 1, 24, false};
        case model::Fps::F25: return {25, 1, 25, false};
        case model::Fps::F29_97: return {30000, 1001, 30, false};
        case model::Fps::F29_97df: return {30000, 1001, 30, true};
        case model::Fps::F30: return {30, 1, 30, false};
    }
    return {25, 1, 25, false};
}

const model::TempoEntry kDefaultTempo{};

// The tempo map's first entry: the fixed step frames_per_beat() reports.
const model::TempoEntry& first_tempo(const model::SessionSettings& s) {
    return s.tempo.empty() ? kDefaultTempo : s.tempo.front();
}

int meter_beats(const model::TempoEntry& t) {
    return t.meter[0] > 0 ? static_cast<int>(t.meter[0]) : 4;
}

double entry_beat_frames(const model::TempoEntry& t, uint32_t rate) {
    const double bpm = t.bpm > 0 ? t.bpm : 120.0;
    const int den = t.meter[1] > 0 ? static_cast<int>(t.meter[1]) : 4;
    return static_cast<double>(rate) * 60.0 / bpm * 4.0 / den;
}

// The map the windows read. A snapshot's settings passed the loader or
// apply(), so they are inside the bounds and build as they are. A map that
// never went through either (a test, a value still being typed) still reads
// rather than going blank: an entry out of order is not a change, the first
// governs from 0, and a bpm or beat outside the bounds is read as the v3
// migration would open it. False only without a usable rate.
//
// A map is short (a song has tens of changes), so it is built per call on
// the GUI thread rather than cached beside a document that can change under
// the cache.
bool reading_map(const model::SessionSettings& s, model::MusicalMap& m) {
    std::string why;
    if (m.build(s, why)) return true;
    if (s.sample_rate == 0 || s.sample_rate > model::kMaxSampleRate) return false;
    std::vector<model::TempoEntry> fixed;
    for (const model::TempoEntry& e : s.tempo) {
        if (!fixed.empty() && (e.at <= fixed.back().at || e.at > model::kMaxInteger)) continue;
        model::TempoEntry f = e;
        if (fixed.empty()) f.at = 0;
        f.bpm = model::bpm_within_bounds(e.bpm);
        if (!(std::isfinite(f.bpm) && f.bpm > 0)) f.bpm = kDefaultTempo.bpm;
        if (f.meter[0] == 0 || f.meter[0] > 0xFFFF) f.meter[0] = kDefaultTempo.meter[0];
        f.meter[1] = model::nearest_meter_denominator(e.meter[1]);
        if (f.meter[1] == 0) f.meter[1] = kDefaultTempo.meter[1];
        fixed.push_back(f);
    }
    if (fixed.empty()) fixed.push_back(kDefaultTempo);
    return m.build(s.sample_rate, fixed, why);
}

MusicalTime reading_of(const model::BarsBeats& b) {
    return {b.bar, static_cast<int>(b.beat), b.tick};
}

// A grid value's step in ticks: whole numbers for every beat the bounds
// allow (a sixteenth of a 1/64 beat is 15,000).
model::Ticks grid_step_ticks(GridValue g, const model::MusicalMap::Entry& e) {
    switch (g) {
        case GridValue::Bar: return static_cast<model::Ticks>(e.beats) * e.beat_ticks;
        case GridValue::Beat: return e.beat_ticks;
        case GridValue::Eighth: return e.beat_ticks / 2;
        case GridValue::Sixteenth: return e.beat_ticks / 4;
    }
    return e.beat_ticks;
}

// Frames a stretch of `ticks` lasts in entry `e`, for spacing only.
double frames_of_ticks(model::Ticks ticks, const model::MusicalMap::Entry& e) {
    return static_cast<double>(ticks) * static_cast<double>(e.den) / static_cast<double>(e.num);
}

model::Ticks ceil_div(model::Ticks a, model::Ticks b) {   // b > 0
    model::Ticks q = a / b;
    if (a % b != 0 && a > 0) ++q;
    return q;
}

// The lines in [a, b] frames on `m`, as grid_lines() states them. With
// `bars_only`, `bar_every` > 1 keeps only the bars whose 0-based number is
// a multiple of it, found directly rather than walked bar by bar, so a view
// of a million bars costs what its labels cost.
void lines_on(const model::MusicalMap& m, double a, double b, GridValue g, bool bars_only,
              std::vector<GridLine>& out, std::size_t max, int64_t bar_every = 1) {
    out.clear();
    // Whole frames only: a line is drawn at the frame nearest it. None
    // before the timeline's 0, where entry 0 would otherwise extend.
    const double lo = std::max(a, 0.0);
    const double hi = std::min(b, static_cast<double>(model::kMaxInteger - 1));
    if (!(hi >= lo)) return;
    const auto fa = static_cast<model::Frames>(std::ceil(lo));
    const auto fb = static_cast<model::Frames>(std::floor(hi));
    model::Ticks ta = 0, tb = 0;
    if (fb < fa || !m.tick_at(fa - 1, ta) || !m.tick_at(fb + 1, tb)) return;
    // One tick either side of the frames' ticks holds every line whose
    // frame is in [fa, fb], whether ticks are finer than frames or not.
    ta = std::max<model::Ticks>(0, ta - 1);
    ++tb;
    const auto& entries = m.entries();
    for (std::size_t k = m.entry_at_tick(ta); k < m.reachable(); ++k) {
        const model::MusicalMap::Entry& e = entries[k];
        if (e.tick > tb) break;
        const model::Ticks end =
            k + 1 < m.reachable() ? entries[k + 1].tick : model::kMaxMusical + 1;
        const model::Ticks bar = static_cast<model::Ticks>(e.beats) * e.beat_ticks;
        const model::Ticks step = bars_only ? bar : grid_step_ticks(g, e);
        const model::Ticks from = std::max(ta, e.tick);
        const model::Ticks to = std::min(tb, end - 1);
        if (bars_only && bar_every > 1) {
            if (to < from) continue;
            // Bars from e.bar_tick are e.bar, e.bar + 1, ...: the first at or
            // after `from`, moved up to a multiple of bar_every. Counted in
            // bars, not ticks, so a long stride cannot overflow.
            const int64_t first = e.bar + ceil_div(from - e.bar_tick, bar);
            const int64_t last = e.bar + (to - e.bar_tick) / bar;
            for (int64_t n = (first + bar_every - 1) / bar_every * bar_every;
                 n <= last && out.size() < max; n += bar_every) {
                const model::Ticks t = e.bar_tick + (n - e.bar) * bar;
                model::Frames f = 0;
                if (!m.frame_at(t, f)) return;
                if (f < fa || f > fb) continue;
                out.push_back({static_cast<double>(f), true, t});
            }
            continue;
        }
        for (model::Ticks t = e.bar_tick + ceil_div(from - e.bar_tick, step) * step;
             t <= to && out.size() < max; t += step) {
            model::Frames f = 0;
            if (!m.frame_at(t, f)) return;
            if (f < fa || f > fb) continue;
            out.push_back({static_cast<double>(f), (t - e.bar_tick) % bar == 0, t});
        }
    }
}

double grid_step_beats(GridValue g, int num) {
    switch (g) {
        case GridValue::Bar: return num;
        case GridValue::Beat: return 1.0;
        case GridValue::Eighth: return 0.5;
        case GridValue::Sixteenth: return 0.25;
    }
    return 1.0;
}

constexpr int64_t kDaySeconds = 24 * 3600;

template <typename... A>
std::string printf_string(const char* fmt, A... a) {
    char buf[48];
    std::snprintf(buf, sizeof buf, fmt, static_cast<long long>(a)...);
    return buf;
}

double beat_frames(const model::SessionSettings& s) {
    return entry_beat_frames(first_tempo(s), s.sample_rate);
}

}  // namespace

std::string format_timecode(model::Frames f, const model::SessionSettings& s) {
    if (s.sample_rate == 0) return "--:--:--:--";
    const FrameRate r = frame_rate(s.timecode.fps);
    const int64_t day = kDaySeconds * s.sample_rate;
    int64_t total = (s.timecode.origin + f) % day;
    if (total < 0) total += day;
    // Video frames since midnight, exact in integers: samples * num / (den * rate).
    // Under 2^47 for a day at 192 kHz times 30000, so int64 holds it exactly.
    int64_t n = total * r.num / (r.den * static_cast<int64_t>(s.sample_rate));
    if (r.drop) {
        // SMPTE drop-frame: labels ;00 and ;01 are skipped every minute
        // except every tenth.
        const int64_t d = n / 17982, m = n % 17982;
        n += 18 * d + (m > 2 ? 2 * ((m - 2) / 1798) : 0);
    }
    const int64_t ff = n % r.base;
    const int64_t secs = n / r.base;
    std::string out = printf_string("%02lld:%02lld:%02lld:%02lld", secs / 3600 % 24,
                                    secs / 60 % 60, secs % 60, ff);
    if (r.drop) out[8] = ';';
    return out;
}

double frames_per_beat(const model::SessionSettings& s) { return beat_frames(s); }

double frames_per_bar(const model::SessionSettings& s) {
    return beat_frames(s) * meter_beats(first_tempo(s));
}

MusicalTime musical_time(model::Frames f, const model::SessionSettings& s) {
    model::MusicalMap m;
    model::Ticks t = 0;
    if (!reading_map(s, m) || !m.tick_at(std::max<model::Frames>(0, f), t)) return {};
    return reading_of(m.bars_beats_at(t));
}

model::Frames frame_of(const MusicalTime& t, const model::SessionSettings& s) {
    model::MusicalMap m;
    model::Ticks tick = 0;
    model::Frames f = 0;
    if (!reading_map(s, m) || !m.tick_of({t.bar, t.beat, t.tick}, tick) || !m.frame_at(tick, f)) {
        return 0;
    }
    return f;
}

std::size_t tempo_index_at(model::Frames f, const model::SessionSettings& s) {
    // As reading_map() reads the map: the first entry governs from 0, and
    // one out of order is not a change.
    std::size_t index = 0;
    model::Frames start = 0;
    for (std::size_t i = 1; i < s.tempo.size(); ++i) {
        if (s.tempo[i].at <= start) continue;
        if (s.tempo[i].at > f) break;
        index = i;
        start = s.tempo[i].at;
    }
    return index;
}

const model::TempoEntry& tempo_at(model::Frames f, const model::SessionSettings& s) {
    if (s.tempo.empty()) return kDefaultTempo;
    return s.tempo[tempo_index_at(f, s)];
}

std::string format_bpm(float bpm) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", static_cast<double>(bpm));
    return buf;
}

std::string format_meter(const model::TempoEntry& t) {
    return std::to_string(t.meter[0]) + "/" + std::to_string(t.meter[1]);
}

namespace {

std::string bars_beats_text(const MusicalTime& t) {
    return printf_string("%lld|%lld|%03lld", t.bar, t.beat, t.tick);
}

// musical_time() on a map already built.
MusicalTime musical_time_on(const model::MusicalMap& m, model::Frames f) {
    model::Ticks t = 0;
    if (!m.tick_at(std::max<model::Frames>(0, f), t)) return {};
    return reading_of(m.bars_beats_at(t));
}

}  // namespace

std::string format_bars_beats(model::Frames f, const model::SessionSettings& s) {
    if (s.sample_rate == 0) return "-|-|---";
    return bars_beats_text(musical_time(f, s));
}

std::vector<std::string> tempo_entry_positions(const model::SessionSettings& s) {
    std::vector<std::string> out;
    out.reserve(s.tempo.size());
    model::MusicalMap m;
    const bool read = s.sample_rate != 0 && reading_map(s, m);
    for (std::size_t i = 0; i < s.tempo.size(); ++i) {
        if (s.sample_rate == 0) {
            out.emplace_back("-|-|---");
        } else {
            out.push_back(bars_beats_text(read ? musical_time_on(m, i == 0 ? 0 : s.tempo[i].at)
                                               : MusicalTime{}));
        }
    }
    return out;
}

std::string format_min_sec(model::Frames f, uint32_t rate) {
    if (rate == 0) return "-:--.---";
    const bool neg = f < 0;
    const int64_t ms = (neg ? -f : f) * 1000 / rate;
    const int64_t s = ms / 1000;
    std::string out = s >= 3600 ? printf_string("%lld:%02lld:%02lld.%03lld", s / 3600,
                                                 s / 60 % 60, s % 60, ms % 1000)
                                : printf_string("%lld:%02lld.%03lld", s / 60, s % 60, ms % 1000);
    return neg ? "-" + out : out;
}

std::string format_samples(model::Frames f) { return std::to_string(f); }

std::string format_time(TimeFormat fmt, model::Frames f, const model::SessionSettings& s) {
    switch (fmt) {
        case TimeFormat::Timecode: return format_timecode(f, s);
        case TimeFormat::BarsBeats: return format_bars_beats(f, s);
        case TimeFormat::MinSec: return format_min_sec(f, s.sample_rate);
        case TimeFormat::Samples: return format_samples(f);
    }
    return {};
}

const char* time_format_name(TimeFormat fmt) {
    switch (fmt) {
        case TimeFormat::Timecode: return "Timecode";
        case TimeFormat::BarsBeats: return "Bars|Beats";
        case TimeFormat::MinSec: return "Min:Secs";
        case TimeFormat::Samples: return "Samples";
    }
    return "";
}

const char* grid_value_name(GridValue g) {
    switch (g) {
        case GridValue::Bar: return "1 bar";
        case GridValue::Beat: return "1/4";
        case GridValue::Eighth: return "1/8";
        case GridValue::Sixteenth: return "1/16";
    }
    return "";
}

GridValue next_grid_value(GridValue g) {
    switch (g) {
        case GridValue::Bar: return GridValue::Beat;
        case GridValue::Beat: return GridValue::Eighth;
        case GridValue::Eighth: return GridValue::Sixteenth;
        case GridValue::Sixteenth: return GridValue::Bar;
    }
    return GridValue::Bar;
}

double grid_frames(GridValue g, const model::SessionSettings& s) {
    return grid_step_beats(g, meter_beats(first_tempo(s))) * frames_per_beat(s);
}

void grid_lines(double a, double b, GridValue g, bool bars_only,
                const model::SessionSettings& s, std::vector<GridLine>& out, std::size_t max) {
    out.clear();
    model::MusicalMap m;
    if (b < a || !reading_map(s, m)) return;
    lines_on(m, a, b, g, bars_only, out, max);
}

model::Frames nudge_target(model::Frames f, int direction, GridValue g,
                           const model::SessionSettings& s) {
    const auto fixed = [&]() {
        const auto step = static_cast<model::Frames>(std::llround(grid_frames(g, s)));
        return std::max<model::Frames>(0, f + direction * step);
    };
    model::MusicalMap m;
    model::Ticks t = 0;
    if (!reading_map(s, m) || !m.tick_at(std::max<model::Frames>(0, f), t)) return fixed();
    // Going back, the step is the one that ends at `t`: the bar before a
    // meter change is the old meter's bar.
    const model::Ticks read_at = direction < 0 ? std::max<model::Ticks>(0, t - 1) : t;
    const std::size_t k = std::min(m.entry_at_tick(read_at), m.reachable() - 1);
    const model::Ticks to = std::max<model::Ticks>(0, t + direction * grid_step_ticks(g, m.entries()[k]));
    model::Frames from_frame = 0, to_frame = 0;
    if (!m.frame_at(t, from_frame) || !m.frame_at(to, to_frame)) return fixed();
    return std::max<model::Frames>(0, f + (to_frame - from_frame));
}

model::Frames snap_to_grid(model::Frames f, GridValue g, const model::SessionSettings& s) {
    model::MusicalMap m;
    if (!reading_map(s, m)) return std::max<model::Frames>(0, f);
    // Far enough either side to hold a line whatever the tempo there: the
    // longest step anywhere on the map.
    double reach = 0;
    for (std::size_t k = 0; k < m.reachable(); ++k) {
        const model::MusicalMap::Entry& e = m.entries()[k];
        reach = std::max(reach, frames_of_ticks(grid_step_ticks(g, e), e));
    }
    std::vector<GridLine> lines;
    const double at = static_cast<double>(f);
    lines_on(m, at - reach * 1.01 - 1, at + reach * 1.01 + 1, g, false, lines, 4096);
    double best = 0;
    double best_d = HUGE_VAL;
    for (const GridLine& l : lines) {
        // Ties go to the later line, as rounding half up does.
        const double d = std::abs(l.frame - at);
        if (d < best_d || (d == best_d && l.frame > best)) {
            best = l.frame;
            best_d = d;
        }
    }
    if (best_d == HUGE_VAL) return std::max<model::Frames>(0, f);
    return std::max<model::Frames>(0, static_cast<model::Frames>(std::llround(best)));
}

namespace {

// Emits majors at multiples of `major` (in `unit`s, where `unit` frames is
// one unit and `offset` frames sits before the timeline's 0), and minors at
// `major / sub` between them, while they stay at least 6 px apart.
template <typename Label>
void emit_ticks(double first, double fpp, double width, double unit, double offset,
                double major, int sub, std::vector<RulerTick>& out, Label label) {
    const double major_frames = major * unit;
    const double a = first + offset;  // in the unit domain's frames
    const double b = a + width * fpp;
    const int64_t k0 = static_cast<int64_t>(std::floor(a / major_frames));
    const int64_t k1 = static_cast<int64_t>(std::ceil(b / major_frames));
    const bool minors = sub > 1 && major_frames / sub / fpp >= 6.0;
    for (int64_t k = k0; k <= k1 && out.size() < 4096; ++k) {
        const double at = static_cast<double>(k) * major_frames;
        const double x = (at - a) / fpp;
        if (x >= -1 && x <= width + 1 && at - offset >= 0) {
            out.push_back({x, true, label(at - offset, k)});
        }
        if (!minors) continue;
        for (int j = 1; j < sub; ++j) {
            const double mx = (at + major_frames * j / sub - a) / fpp;
            if (mx >= 0 && mx <= width && at + major_frames * j / sub - offset >= 0) {
                out.push_back({mx, false, {}});
            }
        }
    }
}

double pick(const double* candidates, int n, double at_least) {
    for (int i = 0; i < n; ++i) {
        if (candidates[i] >= at_least) return candidates[i];
    }
    return candidates[n - 1];
}

// Bars|Beats, through the tempo map. The spacing is chosen where bars are
// shortest in view, so labels never crowd where the tempo is fastest. A bar
// a meter change cut short can still bring a label too close to the last
// one; that line is drawn as a minor tick instead.
void bar_ticks(double first, double fpp, double width, double need, double min_major_px,
               const model::SessionSettings& s, std::vector<RulerTick>& out) {
    const double last = first + width * fpp;
    model::MusicalMap m;
    if (!reading_map(s, m)) return;
    double min_beat = HUGE_VAL, min_bar = HUGE_VAL;
    for (std::size_t k = 0; k < m.reachable(); ++k) {
        const model::MusicalMap::Entry& e = m.entries()[k];
        const double start = static_cast<double>(e.at);
        const double end = k + 1 < m.reachable() ? static_cast<double>(m.entries()[k + 1].at)
                                                  : HUGE_VAL;
        if (end <= first || start > last) continue;
        min_beat = std::min(min_beat, frames_of_ticks(e.beat_ticks, e));
        min_bar = std::min(min_bar, frames_of_ticks(e.beat_ticks * e.beats, e));
    }
    // Labels every 1, 2, 4, ... bars, as far as the view needs: a long
    // session zoomed out labels every 4096th bar rather than losing its
    // labels past the first few thousand bars. 2^40 bars is past any tick
    // the map can reach.
    const bool beat_majors = min_beat >= need;
    int64_t every = 1;
    if (!beat_majors) {
        while (every < (int64_t{1} << 40) && static_cast<double>(every) * min_bar < need) {
            every *= 2;
        }
    }
    // Between the labels: sixteenths under beat labels, beats under bar
    // labels at every bar, bars otherwise; each only if 6 px apart.
    const bool minor_beats = beat_majors ? min_beat / 4 / fpp >= 6.0
                                         : every == 1 && min_beat / fpp >= 6.0;
    const bool minor_bars = !beat_majors && min_bar / fpp >= 6.0;
    const GridValue sub = beat_majors ? GridValue::Sixteenth : GridValue::Beat;
    // Where no bar between the labels is drawn, only the labelled bars are
    // looked for. Every other case draws each line it asks for, at least
    // 6 px apart, so the view's width bounds it.
    std::vector<GridLine> lines;
    const bool bars_only = !beat_majors && !minor_beats;
    lines_on(m, std::max(0.0, first), last, sub, bars_only, lines, 4096,
             bars_only && !minor_bars ? every : 1);
    double last_major = -HUGE_VAL;
    for (const GridLine& l : lines) {
        if (out.size() >= 4096) break;
        const double x = (l.frame - first) / fpp;
        if (x < -1 || x > width + 1) continue;
        const MusicalTime t = reading_of(m.bars_beats_at(l.tick));
        bool major = beat_majors ? t.tick == 0 : l.bar && (t.bar - 1) % every == 0;
        if (major && x - last_major < min_major_px - 1.0) major = false;
        if (major) {
            last_major = x;
            out.push_back({x, true,
                           beat_majors ? std::to_string(t.bar) + "|" + std::to_string(t.beat)
                                       : std::to_string(t.bar)});
        } else if (beat_majors ? minor_beats : (l.bar ? minor_bars : minor_beats)) {
            out.push_back({x, false, {}});
        }
    }
}

}  // namespace

void ruler_ticks(TimeFormat fmt, double first, double fpp, double width,
                 const model::SessionSettings& s, std::vector<RulerTick>& out,
                 double min_major_px) {
    out.clear();
    if (fpp <= 0 || width <= 0 || s.sample_rate == 0) return;
    const double rate = s.sample_rate;
    const double need = min_major_px * fpp;  // frames between majors, at least

    switch (fmt) {
        case TimeFormat::MinSec: {
            static const double kSec[] = {0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5,
                                          10,   15,   30,   60,  120, 300, 600, 1800, 3600};
            const double iv = pick(kSec, sizeof kSec / sizeof kSec[0], need / rate);
            const int sub = iv == 15 || iv == 30 ? 3 : (iv == 2 || iv == 0.2 || iv == 0.02) ? 2 : 5;
            emit_ticks(first, fpp, width, rate, 0.0, iv, sub, out, [&](double at, int64_t) {
                std::string t = format_min_sec(static_cast<model::Frames>(std::llround(at)),
                                               s.sample_rate);
                if (iv >= 1 && t.size() > 4) t.erase(t.size() - 4);  // drop ".000"
                return t;
            });
            break;
        }
        case TimeFormat::Timecode: {
            const FrameRate r = frame_rate(s.timecode.fps);
            const double frame_s = 1.0 / r.base;
            const double kTc[] = {frame_s, 5 * frame_s, 10 * frame_s, 1, 2, 5, 10,
                                  15,      30,          60,           120, 300, 600, 1800, 3600};
            const double iv = pick(kTc, sizeof kTc / sizeof kTc[0], need / rate);
            const double origin = static_cast<double>(s.timecode.origin);
            emit_ticks(first, fpp, width, rate, origin, iv, 5, out,
                       [&](double at, int64_t) {
                           return format_timecode(static_cast<model::Frames>(std::llround(at)), s);
                       });
            break;
        }
        case TimeFormat::BarsBeats:
        case TimeFormat::Samples:
            bar_ticks(first, fpp, width, need, min_major_px, s, out);
            break;
    }
}

}  // namespace sonora::ui
