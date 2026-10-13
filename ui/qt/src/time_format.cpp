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

FrameRate parse_fps(const std::string& fps) {
    if (fps == "23.976") return {24000, 1001, 24, false};
    if (fps == "24") return {24, 1, 24, false};
    if (fps == "29.97") return {30000, 1001, 30, false};
    if (fps == "29.97df") return {30000, 1001, 30, true};
    if (fps == "30") return {30, 1, 30, false};
    return {25, 1, 25, false};
}

constexpr int64_t kDaySeconds = 24 * 3600;

template <typename... A>
std::string printf_string(const char* fmt, A... a) {
    char buf[48];
    std::snprintf(buf, sizeof buf, fmt, static_cast<long long>(a)...);
    return buf;
}

double beat_frames(const model::SessionSettings& s) {
    const double bpm = s.bpm > 0 ? s.bpm : 120.0;
    const int den = s.meter_den > 0 ? s.meter_den : 4;
    return static_cast<double>(s.sample_rate) * 60.0 / bpm * 4.0 / den;
}

}  // namespace

std::string format_timecode(model::Frames f, const model::SessionSettings& s) {
    if (s.sample_rate == 0) return "--:--:--:--";
    const FrameRate r = parse_fps(s.fps);
    const int64_t day = kDaySeconds * s.sample_rate;
    int64_t total = (s.timecode_origin + f) % day;
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
    return beat_frames(s) * (s.meter_num > 0 ? s.meter_num : 4);
}

std::string format_bars_beats(model::Frames f, const model::SessionSettings& s) {
    if (s.sample_rate == 0) return "-|-|---";
    const double fpb = beat_frames(s);
    const int num = s.meter_num > 0 ? s.meter_num : 4;
    const double beats = static_cast<double>(std::max<model::Frames>(0, f)) / fpb;
    // Rounded to the tick first, so a position exactly on a beat never reads
    // as 959 ticks into the one before.
    const int64_t ticks = static_cast<int64_t>(std::floor(beats * 960.0 + 1e-6));
    const int64_t whole = ticks / 960;
    return printf_string("%lld|%lld|%03lld", whole / num + 1, whole % num + 1, ticks % 960);
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
    switch (g) {
        case GridValue::Bar: return frames_per_bar(s);
        case GridValue::Beat: return frames_per_beat(s);
        case GridValue::Eighth: return frames_per_beat(s) / 2;
        case GridValue::Sixteenth: return frames_per_beat(s) / 4;
    }
    return frames_per_beat(s);
}

model::Frames snap_to_grid(model::Frames f, GridValue g, const model::SessionSettings& s) {
    const double step = grid_frames(g, s);
    if (step <= 0) return std::max<model::Frames>(0, f);
    const double k = std::round(static_cast<double>(f) / step);
    return std::max<model::Frames>(0, static_cast<model::Frames>(std::llround(k * step)));
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
            const FrameRate r = parse_fps(s.fps);
            const double frame_s = 1.0 / r.base;
            const double kTc[] = {frame_s, 5 * frame_s, 10 * frame_s, 1, 2, 5, 10,
                                  15,      30,          60,           120, 300, 600, 1800, 3600};
            const double iv = pick(kTc, sizeof kTc / sizeof kTc[0], need / rate);
            const double origin = static_cast<double>(s.timecode_origin);
            emit_ticks(first, fpp, width, rate, origin, iv, 5, out,
                       [&](double at, int64_t) {
                           return format_timecode(static_cast<model::Frames>(std::llround(at)), s);
                       });
            break;
        }
        case TimeFormat::BarsBeats:
        case TimeFormat::Samples: {
            const double beat = beat_frames(s);
            const int num = s.meter_num > 0 ? s.meter_num : 4;
            const double kBeats[] = {1.0 * 1, 1.0 * num, 2.0 * num, 4.0 * num,
                                     8.0 * num, 16.0 * num, 32.0 * num, 64.0 * num};
            const double iv = pick(kBeats, 8, need / beat);
            const int sub = iv == 1 ? 4 : iv == num ? num : 4;
            emit_ticks(first, fpp, width, beat, 0.0, iv, sub, out, [&](double at, int64_t) {
                const int64_t beats = std::llround(at / beat);
                if (iv >= num) return std::to_string(beats / num + 1);
                return std::to_string(beats / num + 1) + "|" + std::to_string(beats % num + 1);
            });
            break;
        }
    }
}

}  // namespace sonora::ui
