// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/pitch/segment.h"

#include <algorithm>
#include <cmath>

namespace sonora::pitch {
namespace {

// The reference a frame is compared with is the median of the last 300 ms of
// the note so far: it follows a slow drift (no split) but not a step.
constexpr std::size_t kReferenceFrames = 60;

struct Run {
    std::size_t first = 0;      // frame index
    std::vector<float> cents;   // gaps filled
};

std::size_t frames_for(double ms, const Contour& c) {
    return std::max<std::size_t>(1, std::size_t(std::lround(ms / c.frame_ms())));
}

double median_of(std::vector<float> v) {
    if (v.empty()) return 0.0;
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + long(mid), v.end());
    double m = v[mid];
    if (v.size() % 2 == 0) {
        const float lower = *std::max_element(v.begin(), v.begin() + long(mid));
        m = 0.5 * (m + lower);
    }
    return m;
}

double percentile(std::vector<double> v, double p) {
    const std::size_t at = std::size_t(std::lround(p * double(v.size() - 1)));
    std::nth_element(v.begin(), v.begin() + long(at), v.end());
    return v[at];
}

// Voiced stretches, joined across unvoiced gaps of at most max_gap frames
// (the gap is filled by a straight line between its neighbours).
std::vector<Run> find_runs(const Contour& c, std::size_t max_gap) {
    std::vector<Run> runs;
    std::size_t i = 0;
    const std::size_t n = c.frames.size();
    while (i < n) {
        if (!c.frames[i].voiced) {
            ++i;
            continue;
        }
        Run run;
        run.first = i;
        run.cents.push_back(c.frames[i].cents);
        std::size_t last = i;
        for (std::size_t j = i + 1; j < n && j - last <= max_gap + 1; ++j) {
            if (!c.frames[j].voiced) continue;
            const float from = c.frames[last].cents, to = c.frames[j].cents;
            for (std::size_t g = last + 1; g < j; ++g) {
                run.cents.push_back(from + (to - from) * float(g - last) / float(j - last));
            }
            run.cents.push_back(to);
            last = j;
        }
        i = last + 1;
        runs.push_back(std::move(run));
    }
    return runs;
}

// Start indices (into the run) of each note: a new one begins where the
// contour has left the reference by split_cents for confirm frames running.
std::vector<std::size_t> split_points(const std::vector<float>& cents, const SegmentConfig& cfg,
                                      const Contour& c) {
    const std::size_t confirm = frames_for(cfg.split_confirm_ms, c);
    std::vector<std::size_t> starts{0};
    std::size_t deviating = 0;
    for (std::size_t i = 1; i < cents.size(); ++i) {
        const std::size_t begin = std::max(starts.back(), i > kReferenceFrames ? i - kReferenceFrames : 0);
        if (begin >= i) continue;  // the note just began: nothing to compare with yet
        const double ref = median_of({cents.begin() + long(begin), cents.begin() + long(i)});
        deviating = std::fabs(double(cents[i]) - ref) > cfg.split_cents ? deviating + 1 : 0;
        if (deviating >= confirm) {
            starts.push_back(i + 1 - deviating);
            deviating = 0;
        }
    }
    return starts;
}

void measure(Note& note) {
    const std::vector<float>& v = note.contour_cents;
    const std::size_t n = v.size();
    note.median_cents = median_of(v);
    double mean_x = 0.5 * double(n - 1), mean_y = 0.0;
    for (const float y : v) mean_y += y;
    mean_y /= double(n);
    double sxy = 0.0, sxx = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        sxy += (double(i) - mean_x) * (v[i] - mean_y);
        sxx += (double(i) - mean_x) * (double(i) - mean_x);
    }
    const double slope = sxx > 0.0 ? sxy / sxx : 0.0;
    note.drift_cents = slope * double(n - 1);
    std::vector<double> residual(n);
    for (std::size_t i = 0; i < n; ++i) residual[i] = v[i] - (mean_y + slope * (double(i) - mean_x));
    note.vibrato_depth_cents = 0.5 * (percentile(residual, 0.95) - percentile(residual, 0.05));
}

Note make_note(const Contour& c, std::size_t first_frame, std::vector<float> cents,
               std::size_t end_boundary) {
    Note note;
    const std::size_t half = c.hop / 2;
    const std::size_t centre = first_frame * c.hop;
    note.start = centre > half ? centre - half : 0;
    note.end = std::min(c.length, end_boundary);
    note.hop = c.hop;
    note.rate = c.rate;
    note.contour_cents = std::move(cents);
    measure(note);
    return note;
}

}  // namespace

bool segment_notes(const Contour& c, const SegmentConfig& cfg, std::vector<Note>& notes,
                   std::string& why) {
    notes.clear();
    const double voiced_ms = double(c.voiced_count()) * c.frame_ms();
    if (voiced_ms < cfg.min_voiced_ms) {
        why = "pitch: too few voiced frames (" + std::to_string(long(voiced_ms)) +
              " ms voiced, at least " + std::to_string(long(cfg.min_voiced_ms)) +
              " ms needed); the audio is not a single pitched voice";
        return false;
    }
    const std::size_t min_frames = frames_for(cfg.min_note_ms, c);
    const std::size_t half = c.hop / 2;
    for (const Run& run : find_runs(c, frames_for(cfg.max_gap_ms, c))) {
        const std::vector<std::size_t> starts = split_points(run.cents, cfg, c);
        for (std::size_t k = 0; k < starts.size(); ++k) {
            const std::size_t from = starts[k];
            const std::size_t to = k + 1 < starts.size() ? starts[k + 1] : run.cents.size();
            if (to - from < min_frames) continue;
            std::vector<float> cents(run.cents.begin() + long(from), run.cents.begin() + long(to));
            // A note's end is the next frame's start, so split notes touch.
            const std::size_t end = (run.first + to - 1) * c.hop + half;
            notes.push_back(make_note(c, run.first + from, std::move(cents),
                                      k + 1 < starts.size() ? (run.first + to) * c.hop - half : end));
        }
    }
    if (notes.empty()) {
        why = "pitch: no note lasts " + std::to_string(long(cfg.min_note_ms)) + " ms or more";
        return false;
    }
    return true;
}

}  // namespace sonora::pitch
