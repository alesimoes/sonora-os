#include "sonora/edit_tools/edit_tools.h"

#include <cmath>
#include <cstdio>

using namespace sonora::edit_tools;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static bool close_to(double a, double b) { return std::fabs(a - b) < 1e-9; }

int main() {
    View v; v.fpp = 100; v.width_px = 1000; v.track_rows = 8;
    Limits lim; lim.session_frames = 1000000; lim.min_fpp = 0.5; lim.track_count = 8;

    // Box: x 100..300 px, rows 2..4, corners given reversed.
    View b = zoom_rect(300, 4, 100, 2, v, lim);
    CHECK(close_to(b.start_frame, 10000));
    CHECK(close_to(b.fpp, 20));
    CHECK(close_to(b.track_top, 2) && close_to(b.track_rows, 2));

    // Box clamps to the session end.
    View late = v; late.start_frame = 899999;
    View c = zoom_rect(900, 0, 1000, 8, late, lim);
    CHECK(c.start_frame + c.fpp * c.width_px <= lim.session_frames + 1e-6);

    // Minimum zoom.
    CHECK(close_to(zoom_rect(0, 0, 4, 8, v, lim).fpp, 0.5));
    // Degenerate box is a no-op.
    View e = zoom_rect(10, 0, 11, 8, v, lim);
    CHECK(close_to(e.fpp, 100) && close_to(e.start_frame, 0));

    // Click: doubles zoom, the frame under the pointer stays put.
    View z = zoom_click(v, 500, false, lim);
    CHECK(close_to(z.fpp, 50));
    CHECK(close_to(z.start_frame + 500 * z.fpp, 50000));
    // Alt-click out, clamped at the session width.
    CHECK(close_to(zoom_click(z, 500, true, lim).fpp, 100));
    View wide = zoom_click(v, 500, true, lim);
    wide = zoom_click(wide, 500, true, lim);
    wide = zoom_click(wide, 500, true, lim);
    wide = zoom_click(wide, 500, true, lim);
    CHECK(close_to(wide.fpp, 1000) && close_to(wide.start_frame, 0));
    // Click in stops at the minimum.
    View tiny = v; tiny.fpp = 0.6;
    CHECK(close_to(zoom_click(tiny, 0, false, lim).fpp, 0.5));

    // Scrub: 100 px over 1 s at 100 fpp, 48 kHz.
    CHECK(close_to(scrub_rate(100, 1000000, 100, 48000), 10000.0 / 48000));
    CHECK(close_to(scrub_rate(1, 1000000, 100, 48000), 0));      // dead zone
    CHECK(close_to(scrub_rate(-1.9, 1000, 100, 48000), 0));
    CHECK(close_to(scrub_rate(1000, 1000, 100, 48000), 4));      // clamp +
    CHECK(close_to(scrub_rate(-1000, 1000, 100, 48000), -4));    // clamp -
    CHECK(scrub_rate(-100, 1000000, 100, 48000) < 0);            // reverse
    CHECK(close_to(scrub_rate(100, 0, 100, 48000), 0));
    CHECK(close_to(scrub_rate(100, 1000, 100, 0), 0));

    // Position clamps to the session.
    CHECK(scrub_position(v, 10, 1e6) == 1000);
    CHECK(scrub_position(v, -50, 1e6) == 0);
    CHECK(scrub_position(v, 1e6, 1e6) == 1000000);

    if (failures == 0) std::printf("edit_tools ok\n");
    return failures ? 1 : 0;
}
