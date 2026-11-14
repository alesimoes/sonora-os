#include "sonora/edit_tools/edit_tools.h"

#include <algorithm>
#include <cmath>

namespace sonora::edit_tools {
namespace {

double clamp(double v, double lo, double hi) { return std::min(std::max(v, lo), hi); }

// Keeps the window inside [0, session_frames]; wider than the session shrinks
// the zoom to fit, but never below min_fpp.
View fit_time(View v, const Limits& lim) {
    v.fpp = std::max(v.fpp, lim.min_fpp);
    if (lim.session_frames > 0 && v.fpp * v.width_px > lim.session_frames) {
        v.fpp = std::max(lim.session_frames / v.width_px, lim.min_fpp);
    }
    const double max_start = std::max(0.0, lim.session_frames - v.fpp * v.width_px);
    v.start_frame = clamp(v.start_frame, 0.0, max_start);
    return v;
}

View fit_rows(View v, const Limits& lim) {
    v.track_rows = clamp(v.track_rows, 1.0, std::max(1.0, lim.track_count));
    v.track_top = clamp(v.track_top, 0.0, std::max(0.0, lim.track_count - v.track_rows));
    return v;
}

}  // namespace

View zoom_rect(double x0, double y0, double x1, double y1,
               const View& view, const Limits& lim) {
    const double xa = std::min(x0, x1), xb = std::max(x0, x1);
    const double ya = std::min(y0, y1), yb = std::max(y0, y1);
    if (xb - xa < kDeadZonePx || view.width_px <= 0) return view;
    View v = view;
    v.start_frame = view.start_frame + xa * view.fpp;
    v.fpp = (xb - xa) * view.fpp / view.width_px;
    v = fit_time(v, lim);
    if (yb - ya > 0) {
        v.track_top = ya;
        v.track_rows = yb - ya;
    }
    return fit_rows(v, lim);
}

View zoom_click(const View& view, double x, bool zoom_out, const Limits& lim) {
    View v = view;
    const double anchor = view.start_frame + x * view.fpp;
    v.fpp = zoom_out ? view.fpp * kClickStep : view.fpp / kClickStep;
    v.fpp = std::max(v.fpp, lim.min_fpp);
    v.start_frame = anchor - x * v.fpp;
    return fit_time(v, lim);
}

double scrub_rate(double dx_pixels, int64_t dt_us, double fpp, double rate) {
    if (dt_us <= 0 || rate <= 0 || std::fabs(dx_pixels) < kDeadZonePx) return 0.0;
    const double frames_per_s = dx_pixels * fpp / (static_cast<double>(dt_us) * 1e-6);
    return clamp(frames_per_s / rate, -kMaxScrubSpeed, kMaxScrubSpeed);
}

int64_t scrub_position(const View& view, double x, double session_frames) {
    const double f = clamp(view.start_frame + x * view.fpp, 0.0, std::max(0.0, session_frames));
    return static_cast<int64_t>(std::llround(f));
}

}  // namespace sonora::edit_tools
