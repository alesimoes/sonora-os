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

// Half a tick, in beats: closer than this to a bar line is on it. Bar lines
// fall on fractional frames, so an entry placed on one by frame is a
// fraction of a frame off it.
constexpr double kOnLine = 0.5 / 960.0;

// One entry's stretch of the timeline and where the count stands at its
// start. A map is short (a song has tens of changes), so these are built per
// call on the GUI thread rather than cached beside a document that can
// change under the cache.
struct Segment {
    double start = 0;       // frames
    double end = 0;         // frames; +inf for the last
    double beat = 0;        // frames per beat
    int num = 4;            // beats per bar
    int64_t bar0 = 0;       // 0-based bar at `start`
    double beat0 = 0;       // beats into that bar at `start`, [0, num)
    std::size_t index = 0;  // into SessionSettings::tempo
};

void build_segments(const model::SessionSettings& s, std::vector<Segment>& out) {
    out.clear();
    const uint32_t rate = s.sample_rate;
    if (s.tempo.empty()) {
        out.push_back({0, HUGE_VAL, entry_beat_frames(kDefaultTempo, rate), 4, 0, 0, 0});
        return;
    }
    for (std::size_t i = 0; i < s.tempo.size(); ++i) {
        const model::TempoEntry& e = s.tempo[i];
        Segment g;
        // The format puts the first entry at 0; one that is not still governs
        // from 0, rather than leaving the start of the session uncounted.
        g.start = i == 0 ? 0.0 : static_cast<double>(e.at);
        g.end = HUGE_VAL;
        g.beat = entry_beat_frames(e, rate);
        g.num = meter_beats(e);
        g.index = i;
        if (!out.empty()) {
            Segment& p = out.back();
            if (g.start <= p.start) continue;  // unsorted or duplicate: not a change
            p.end = g.start;
            const double total = p.beat0 + (g.start - p.start) / p.beat;
            double whole = std::floor(total / p.num);
            double into = total - whole * p.num;
            if (into > p.num - kOnLine) {
                whole += 1;
                into = 0;
            } else if (into < kOnLine) {
                into = 0;
            }
            g.bar0 = p.bar0 + static_cast<int64_t>(whole);
            const bool meter_changes = s.tempo[p.index].meter != e.meter;
            if (meter_changes && into > 0) {
                // The bar the change cuts short is counted; the new meter's
                // first bar starts here.
                g.bar0 += 1;
                into = 0;
            }
            g.beat0 = into;
        }
        out.push_back(g);
    }
}

std::size_t segment_at(const std::vector<Segment>& segs, double f) {
    std::size_t i = 0;
    while (i + 1 < segs.size() && segs[i + 1].start <= f) ++i;
    return i;
}

MusicalTime musical_time_on(const std::vector<Segment>& segs, double at) {
    const Segment& g = segs[segment_at(segs, at)];
    const double beats = g.beat0 + (at - g.start) / g.beat;
    // Rounded to the tick first, so a position exactly on a beat never reads
    // as 959 ticks into the one before.
    const int64_t ticks = static_cast<int64_t>(std::floor(beats * 960.0 + 1e-6));
    const int64_t whole = ticks / 960;
    MusicalTime t;
    t.bar = g.bar0 + whole / g.num + 1;
    t.beat = static_cast<int>(whole % g.num) + 1;
    t.tick = static_cast<int>(ticks % 960);
    return t;
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
    if (s.sample_rate == 0) return {};
    std::vector<Segment> segs;
    build_segments(s, segs);
    return musical_time_on(segs, static_cast<double>(std::max<model::Frames>(0, f)));
}

model::Frames frame_of(const MusicalTime& t, const model::SessionSettings& s) {
    if (s.sample_rate == 0) return 0;
    std::vector<Segment> segs;
    build_segments(s, segs);
    for (const Segment& g : segs) {
        // A bar that ended before this segment began, and was not reached in
        // the one before: a meter change cut it short, and what is left of it
        // is the change's own frame, not a place inside the new meter's bars.
        if (t.bar - 1 < g.bar0) return static_cast<model::Frames>(std::llround(g.start));
        const double x = static_cast<double>(t.bar - 1 - g.bar0) * g.num + (t.beat - 1) +
                         t.tick / 960.0 - g.beat0;
        if (x < 0) return static_cast<model::Frames>(std::llround(g.start));
        const double f = g.start + x * g.beat;
        if (f < g.end) return static_cast<model::Frames>(std::llround(f));
    }
    return 0;
}

std::size_t tempo_index_at(model::Frames f, const model::SessionSettings& s) {
    std::vector<Segment> segs;
    build_segments(s, segs);
    return segs[segment_at(segs, static_cast<double>(f))].index;
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

std::string format_bars_beats(model::Frames f, const model::SessionSettings& s) {
    if (s.sample_rate == 0) return "-|-|---";
    const MusicalTime t = musical_time(f, s);
    return printf_string("%lld|%lld|%03lld", t.bar, t.beat, t.tick);
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
    if (s.sample_rate == 0 || b < a) return;
    std::vector<Segment> segs;
    build_segments(s, segs);
    for (const Segment& seg : segs) {
        if (seg.end <= a) continue;
        if (seg.start > b) break;
        const double step = bars_only ? seg.num : grid_step_beats(g, seg.num);
        // In beats from the start of the segment's first bar.
        const double from = (std::max(a, seg.start) - seg.start) / seg.beat + seg.beat0;
        const double to = (std::min(b, seg.end) - seg.start) / seg.beat + seg.beat0;
        for (auto k = static_cast<int64_t>(std::ceil(from / step - 1e-9));
             k * step <= to + 1e-9 && out.size() < max; ++k) {
            const double beats = static_cast<double>(k) * step;
            const double f = seg.start + (beats - seg.beat0) * seg.beat;
            if (f >= seg.end - 1e-6 || f > b || f < a) continue;
            const double in_bar = std::fmod(beats, static_cast<double>(seg.num));
            out.push_back({f, in_bar < kOnLine || in_bar > seg.num - kOnLine});
        }
    }
}

model::Frames snap_to_grid(model::Frames f, GridValue g, const model::SessionSettings& s) {
    if (s.sample_rate == 0) return std::max<model::Frames>(0, f);
    // Far enough either side to hold a line whatever the tempo there: the
    // longest step anywhere on the map.
    std::vector<Segment> segs;
    build_segments(s, segs);
    double reach = 0;
    for (const Segment& seg : segs) reach = std::max(reach, grid_step_beats(g, seg.num) * seg.beat);
    std::vector<GridLine> lines;
    const double at = static_cast<double>(f);
    grid_lines(at - reach * 1.01, at + reach * 1.01, g, false, s, lines);
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
    std::vector<Segment> segs;
    build_segments(s, segs);
    double min_beat = HUGE_VAL, min_bar = HUGE_VAL;
    for (const Segment& seg : segs) {
        if (seg.end <= first || seg.start > last) continue;
        min_beat = std::min(min_beat, seg.beat);
        min_bar = std::min(min_bar, seg.beat * seg.num);
    }
    static const int kEvery[] = {1, 2, 4, 8, 16, 32, 64, 128, 256};
    const bool beat_majors = min_beat >= need;
    int every = 256;
    if (!beat_majors) {
        for (int k : kEvery) {
            if (k * min_bar >= need) {
                every = k;
                break;
            }
        }
    }
    // Between the labels: sixteenths under beat labels, beats under bar
    // labels at every bar, bars otherwise; each only if 6 px apart.
    const bool minor_beats = beat_majors ? min_beat / 4 / fpp >= 6.0
                                         : every == 1 && min_beat / fpp >= 6.0;
    const bool minor_bars = !beat_majors && min_bar / fpp >= 6.0;
    const GridValue sub = beat_majors ? GridValue::Sixteenth : GridValue::Beat;
    std::vector<GridLine> lines;
    grid_lines(std::max(0.0, first), last, sub, !beat_majors && !minor_beats, s, lines);
    double last_major = -HUGE_VAL;
    for (const GridLine& l : lines) {
        if (out.size() >= 4096) break;
        const double x = (l.frame - first) / fpp;
        if (x < -1 || x > width + 1) continue;
        const MusicalTime t = musical_time_on(segs, l.frame);
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
