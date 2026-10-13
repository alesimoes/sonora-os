// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a meter shows: scale, ballistics, and the rectangles that reach the
// scene graph. No display; meter_model has no Qt in it.
#include <cmath>
#include <cstdio>

#include "meter_model.h"
#include "synthetic_engine.h"

using namespace sonora::ui;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

static bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

static float db(float d) { return std::pow(10.0f, d / 20.0f); }

// The rectangle meter_rects() emits in slot `i`. The slots are fixed:
// 0 background, 1 over band, 2 green, 3 amber, 4 red, 5 hold, 6.. marks.
static MeterRect rect(const MeterBallistics& m, int i, float w = 20, float h = 400) {
    MeterRect r[kMeterMaxRects];
    const int n = meter_rects(m, w, h, r);
    return i < n ? r[i] : MeterRect{};
}

int main() {
    // --- dBFS ---
    {
        CHECK(near(peak_to_dbfs(1.0f), 0.0f));
        CHECK(near(peak_to_dbfs(0.5f), -6.0206f));
        CHECK(peak_to_dbfs(0.0f) == kSilenceDb);
        CHECK(peak_to_dbfs(-1.0f) == kSilenceDb);
        CHECK(peak_to_dbfs(std::nanf("")) == kSilenceDb);
        CHECK(peak_to_dbfs(1e-30f) == kSilenceDb);  // clamped, never below
        CHECK(peak_to_dbfs(2.0f) > 0.0f);           // a float can go over
    }

    // --- the scale: anchored, monotonic, clamped ---
    {
        CHECK(dbfs_to_fraction(0.0f) == 1.0f);
        CHECK(dbfs_to_fraction(3.0f) == 1.0f);
        CHECK(dbfs_to_fraction(kMeterFloorDb) == 0.0f);
        CHECK(dbfs_to_fraction(kSilenceDb) == 0.0f);
        CHECK(near(dbfs_to_fraction(-20.0f), 0.5f));  // the top 20 dB get half
        float prev = -1.0f;
        for (float d = -80.0f; d <= 6.0f; d += 0.25f) {
            const float f = dbfs_to_fraction(d);
            CHECK(f >= prev);
            CHECK(f >= 0.0f && f <= 1.0f);
            prev = f;
        }
        CHECK(dbfs_to_fraction(-0.1f) < 1.0f);  // one tenth below full scale is not full scale
    }

    // --- attack is instant: a peak is never shown low ---
    {
        MeterBallistics m;
        m.feed(db(-12.0f), 1.0f / 60);
        CHECK(near(m.bar_db, -12.0f));
        CHECK(near(m.hold_db, -12.0f));
        CHECK(m.has_data);
        CHECK(!m.over);
    }

    // --- release: the bar falls at 20 dB per 1.7 s, no faster ---
    {
        MeterBallistics m;
        m.feed(db(-6.0f), 0.0f);
        for (int i = 0; i < 60; ++i) m.feed(0.0f, 1.0f / 60);  // one second of silence
        CHECK(near(m.bar_db, -6.0f - 20.0f / 1.7f, 0.05f));
        // ...and the hold marker has not moved: its two seconds are not up.
        CHECK(near(m.hold_db, -6.0f));
    }

    // --- peak hold: holds for hold_s, then falls, never below the bar ---
    {
        MeterBallistics m;
        m.feed(db(-3.0f), 0.0f);
        for (int i = 0; i < 119; ++i) m.feed(db(-30.0f), 1.0f / 60);  // 1.98 s
        CHECK(near(m.hold_db, -3.0f));
        for (int i = 0; i < 30; ++i) m.feed(db(-30.0f), 1.0f / 60);   // past 2 s
        CHECK(m.hold_db < -3.0f);
        for (int i = 0; i < 600; ++i) m.feed(db(-30.0f), 1.0f / 60);  // long after
        CHECK(near(m.hold_db, -30.0f, 0.01f));
        CHECK(m.hold_db >= m.bar_db);
        // A new, higher peak restarts the hold.
        m.feed(db(-10.0f), 1.0f / 60);
        CHECK(near(m.hold_db, -10.0f));
        CHECK(m.hold_age_s == 0.0f);
    }

    // --- over: latched at full scale, kept until cleared ---
    {
        MeterBallistics m;
        m.feed(db(-0.01f), 1.0f / 60);
        CHECK(!m.over);  // just below full scale is not an over
        m.feed(1.0f, 1.0f / 60);
        CHECK(m.over);
        for (int i = 0; i < 6000; ++i) m.feed(0.0f, 1.0f / 60);  // 100 s of silence
        CHECK(m.over);
        m.set_no_data();
        CHECK(m.over);  // the engine going away does not unhappen a clip
        m.clear_over();
        CHECK(!m.over);
    }

    // --- no data: nothing frozen, and nothing to fall from afterwards ---
    {
        MeterBallistics m;
        m.feed(db(-1.0f), 0.0f);
        m.set_no_data();
        CHECK(!m.has_data);
        CHECK(m.bar_db == kSilenceDb);
        CHECK(rect(m, 2).h == 0.0f && rect(m, 3).h == 0.0f && rect(m, 4).h == 0.0f);
        CHECK(rect(m, 5).h == 0.0f);  // no hold marker either
        m.feed(db(-40.0f), 5.0f);    // back, after a long gap
        CHECK(near(m.bar_db, -40.0f));
        CHECK(near(m.hold_db, -40.0f));
    }

    // --- geometry: value -> rectangles ---
    {
        const float w = 20, h = 400;
        MeterRect r[kMeterMaxRects];
        MeterBallistics m;
        CHECK(meter_rects(m, w, h, r) == kMeterMaxRects);  // fixed, always
        CHECK(meter_rects(m, 0, h, r) == 0);

        const float top = meter_over_band(h) + 1.0f;
        const float bottom = h;
        // 0 dBFS reaches the top of the scale and lights every zone.
        m.feed(1.0f, 0.0f);
        CHECK(near(rect(m, 4).y, top));                      // red starts at 0 dBFS
        CHECK(near(rect(m, 2).y + rect(m, 2).h, bottom));    // green reaches the floor
        CHECK(rect(m, 1).argb != MeterRect{}.argb);
        // The zones tile the bar with no gap and no overlap.
        CHECK(near(rect(m, 4).y + rect(m, 4).h, rect(m, 3).y));
        CHECK(near(rect(m, 3).y + rect(m, 3).h, rect(m, 2).y));

        // -20 dBFS: half the scale, all green.
        MeterBallistics q;
        q.feed(db(-20.0f), 0.0f);
        const float half = top + (bottom - top) * 0.5f;
        CHECK(near(rect(q, 2).y, half, 0.01f));
        CHECK(rect(q, 3).h == 0.0f);
        CHECK(rect(q, 4).h == 0.0f);
        // The hold marker sits on the peak.
        CHECK(std::fabs(rect(q, 5).y + 1.0f - half) < 0.01f);

        // -10 dBFS: green full, amber partly, no red.
        MeterBallistics a;
        a.feed(db(-10.0f), 0.0f);
        CHECK(rect(a, 3).h > 0.0f);
        CHECK(rect(a, 4).h == 0.0f);

        // The bar never leaves the item, whatever comes in.
        MeterBallistics big;
        big.feed(1000.0f, 0.0f);
        for (int i = 0; i < kMeterMaxRects; ++i) {
            const MeterRect x = rect(big, i, w, h);
            CHECK(x.y >= 0.0f && x.y + x.h <= h + 0.001f);
            CHECK(x.x >= 0.0f && x.x + x.w <= w + 0.001f);
        }
    }

    // --- the synthetic source's peak is a maximum over the interval ---
    {
        uint64_t now = 1'000'000'000;
        SyntheticEngine e(18, [&now]() { return now; });
        std::string err;
        CHECK(e.connect(err));
        // Channel 10 (index 9) goes over full scale in the first 50 ms of
        // every 7 s. A reader that only sampled its tick's end would miss it.
        now += 30'000'000;   // 30 ms in: inside the burst
        now += 40'000'000;   // read at 70 ms, after it ended
        CHECK(SyntheticEngine::level_at(9, 0.07) < 1.0f);
        CHECK(e.take_peak(9) >= 1.0f);
        now += 16'000'000;
        CHECK(e.take_peak(9) < 1.0f);  // the cursor moved on
    }

    std::printf("ui meter model: all checks passed\n");
    return 0;
}
