// SPDX-License-Identifier: GPL-3.0-or-later
#include "meter_model.h"

#include <algorithm>
#include <cmath>

namespace sonora::ui {

namespace {

struct ScalePoint {
    float db;
    float fraction;
};

// The deflection table. Its shape is ours (ADR-0004: workflow, not visual
// design); what it keeps from broadcast practice is that the top of the
// scale gets the room.
constexpr ScalePoint kScale[] = {
    {-60.0f, 0.00f}, {-50.0f, 0.06f}, {-40.0f, 0.15f}, {-30.0f, 0.30f},
    {-20.0f, 0.50f}, {-12.0f, 0.68f}, {-6.0f, 0.83f},  {0.0f, 1.00f},
};

constexpr uint32_t kBackground = 0xff15181c;
constexpr uint32_t kNoDataBackground = 0xff2a2d31;
constexpr uint32_t kGreen = 0xff3fbf7f;
constexpr uint32_t kAmber = 0xffe0b040;
constexpr uint32_t kRed = 0xffe5484d;
constexpr uint32_t kHold = 0xffeef1f4;
constexpr uint32_t kOverOff = 0xff30343a;
constexpr uint32_t kOverOn = 0xffff2d2d;
constexpr uint32_t kMark = 0xff5a6068;

}  // namespace

const float kScaleMarksDb[kScaleMarkCount] = {0.0f,   -6.0f,  -12.0f, -18.0f,
                                              -24.0f, -30.0f, -40.0f, -50.0f};

float peak_to_dbfs(float peak) {
    if (!(peak > 0.0f)) return kSilenceDb;  // also catches NaN
    return std::max(kSilenceDb, 20.0f * std::log10(peak));
}

float dbfs_to_fraction(float db) {
    if (!(db > kScale[0].db)) return 0.0f;
    constexpr int n = sizeof kScale / sizeof kScale[0];
    if (db >= kScale[n - 1].db) return 1.0f;
    for (int i = 1; i < n; ++i) {
        if (db <= kScale[i].db) {
            const ScalePoint& a = kScale[i - 1];
            const ScalePoint& b = kScale[i];
            return a.fraction + (db - a.db) / (b.db - a.db) * (b.fraction - a.fraction);
        }
    }
    return 1.0f;
}

void MeterBallistics::feed(float peak, float dt_s) {
    const float db = peak_to_dbfs(peak);
    if (dt_s < 0.0f) dt_s = 0.0f;
    if (!has_data) {
        // Coming back from "no data", nothing that was on screen before is
        // worth falling from.
        bar_db = kSilenceDb;
        hold_db = kSilenceDb;
        hold_age_s = 0.0f;
        has_data = true;
    }
    if (peak >= 1.0f) over = true;

    bar_db = std::max(db, bar_db - release_db_per_s * dt_s);
    if (bar_db < kSilenceDb) bar_db = kSilenceDb;

    if (db >= hold_db) {
        hold_db = db;
        hold_age_s = 0.0f;
    } else {
        hold_age_s += dt_s;
        if (hold_age_s > hold_s) {
            hold_db = std::max(bar_db, hold_db - release_db_per_s * dt_s);
        }
    }
}

void MeterBallistics::set_no_data() {
    has_data = false;
    bar_db = kSilenceDb;
    hold_db = kSilenceDb;
    hold_age_s = 0.0f;
    // `over` is kept: a clip that happened before the engine went away still
    // happened, and the operator has not seen it cleared.
}

float meter_over_band(float h) { return std::max(2.0f, std::min(8.0f, h * 0.025f)); }

int meter_rects(const MeterBallistics& m, float w, float h, MeterRect* out) {
    int n = 0;
    const auto add = [&](float x, float y, float rw, float rh, uint32_t argb) {
        out[n++] = MeterRect{x, y, std::max(0.0f, rw), std::max(0.0f, rh), argb};
    };
    if (w <= 0.0f || h <= 0.0f) return 0;

    const float over_h = meter_over_band(h);
    const float gap = 1.0f;
    const float top = over_h + gap;            // the scale's 0 dBFS line
    const float span = std::max(0.0f, h - top);  // the scale's height
    const float mark_w = std::max(2.0f, std::floor(w * 0.2f));
    const float bar_w = std::max(1.0f, w - mark_w - 1.0f);
    const auto y_of = [&](float db) { return top + span * (1.0f - dbfs_to_fraction(db)); };

    add(0, 0, w, h, m.has_data ? kBackground : kNoDataBackground);
    add(0, 0, w, over_h, m.over ? kOverOn : kOverOff);

    if (m.has_data) {
        const float bar_top = y_of(m.bar_db);
        const float bottom = top + span;
        const float amber_y = y_of(kAmberFromDb);
        const float red_y = y_of(kRedFromDb);
        // Zone by zone, bottom up; each is clipped to where the bar reaches.
        const float green_top = std::max(bar_top, amber_y);
        add(0, green_top, bar_w, bottom - green_top, kGreen);
        const float amber_top = std::max(bar_top, red_y);
        add(0, amber_top, bar_w, std::max(0.0f, amber_y - amber_top), kAmber);
        add(0, bar_top, bar_w, std::max(0.0f, red_y - bar_top), kRed);
        if (m.hold_db > kMeterFloorDb) {
            const float hy = y_of(m.hold_db);
            add(0, std::max(top, hy - 1.0f), bar_w, 2.0f, kHold);
        } else {
            add(0, 0, 0, 0, kHold);
        }
    } else {
        for (int i = 0; i < 4; ++i) add(0, 0, 0, 0, 0);
    }
    for (int i = 0; i < kScaleMarkCount; ++i) {
        const float y = y_of(kScaleMarksDb[i]);
        add(w - mark_w, std::floor(y), mark_w, 1.0f, kMark);
    }
    return n;
}

}  // namespace sonora::ui
