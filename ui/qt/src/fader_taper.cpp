// SPDX-License-Identifier: GPL-3.0-or-later
#include "fader_taper.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sonora::ui {

namespace {

struct TaperPoint {
    float db;
    float pos;
};

constexpr TaperPoint kTaper[] = {
    {kFaderSilenceDb, 0.00f}, {-60.0f, 0.08f}, {-40.0f, 0.20f}, {-30.0f, 0.30f},
    {-20.0f, 0.42f},          {-10.0f, 0.58f}, {-5.0f, 0.68f},  {0.0f, 0.78f},
    {6.0f, 0.90f},            {12.0f, 1.00f},
};
constexpr int kPoints = sizeof kTaper / sizeof kTaper[0];

}  // namespace

float fader_position_from_db(float db) {
    if (!(db > kTaper[0].db)) return 0.0f;  // also NaN
    if (db >= kTaper[kPoints - 1].db) return 1.0f;
    for (int i = 1; i < kPoints; ++i) {
        if (db <= kTaper[i].db) {
            const TaperPoint& a = kTaper[i - 1];
            const TaperPoint& b = kTaper[i];
            return a.pos + (db - a.db) / (b.db - a.db) * (b.pos - a.pos);
        }
    }
    return 1.0f;
}

float fader_db_from_position(float p) {
    if (!(p > 0.0f)) return kFaderSilenceDb;
    if (p >= 1.0f) return kTaper[kPoints - 1].db;
    for (int i = 1; i < kPoints; ++i) {
        if (p <= kTaper[i].pos) {
            const TaperPoint& a = kTaper[i - 1];
            const TaperPoint& b = kTaper[i];
            return a.db + (p - a.pos) / (b.pos - a.pos) * (b.db - a.db);
        }
    }
    return kTaper[kPoints - 1].db;
}

std::string gain_text(float db) {
    if (!(db > kFaderSilenceDb + 0.05f)) return "-inf";
    const float r = std::round(db * 10.0f) / 10.0f;
    char buf[16];
    if (r == 0.0f) return "0.0";
    std::snprintf(buf, sizeof buf, "%+.1f", static_cast<double>(r));
    return buf;
}

std::string pan_text(float pan) {
    const int v = static_cast<int>(std::lround(std::clamp(pan, -1.0f, 1.0f) * 100.0f));
    if (v == 0) return "C";
    return v < 0 ? "<" + std::to_string(-v) : std::to_string(v) + ">";
}

float fader_drag(float db, float dy, float travel, bool fine) {
    if (travel <= 0) return db;
    const float dp = -dy / travel * (fine ? 0.1f : 1.0f);
    return fader_db_from_position(std::clamp(fader_position_from_db(db) + dp, 0.0f, 1.0f));
}

}  // namespace sonora::ui
