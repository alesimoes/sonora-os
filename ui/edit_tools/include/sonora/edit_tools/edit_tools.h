#pragma once
// Pure view-tool logic (ADR-0158): no Qt, no engine, no clock.
#include <cstdint>

namespace sonora::edit_tools {

// What the edit window shows. x is measured in pixels from the left edge of
// the timeline; y is in track rows.
struct View {
    double start_frame = 0;     // frame at x = 0
    double fpp = 1;             // frames per pixel
    double width_px = 1;        // timeline width
    double track_top = 0;       // first visible track (fractional rows)
    double track_rows = 1;      // visible rows
};

struct Limits {
    double session_frames = 0;  // zoom never shows past this
    double min_fpp = 1.0 / 16;  // deepest zoom
    double track_count = 1;
};

inline constexpr double kClickStep = 2.0;       // zoom factor per click
inline constexpr double kMaxScrubSpeed = 4.0;   // +-4x
inline constexpr double kDeadZonePx = 2.0;      // movement below this is no scrub

// Drag a box (any corner order) and fit the view to it. A box narrower than
// the dead zone returns the view unchanged: that is a click, see zoom_click.
View zoom_rect(double x0, double y0, double x1, double y1,
               const View& view, const Limits& lim);

// Zooms in about x (the frame under x stays under x); out when zoom_out.
View zoom_click(const View& view, double x, bool zoom_out, const Limits& lim);

// Signed playback speed (1.0 = normal) for a pointer move of dx_pixels over
// dt_us; rate is the session sample rate. Zero inside the dead zone, for a
// non-positive dt_us or rate.
double scrub_rate(double dx_pixels, int64_t dt_us, double fpp, double rate);

// Playhead frame under pointer x, clamped to [0, session_frames].
int64_t scrub_position(const View& view, double x, double session_frames);

}  // namespace sonora::edit_tools
