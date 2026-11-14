// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/ui_pitch/pitch_view.h"

#include <algorithm>
#include <cmath>

namespace sonora::ui_pitch {
namespace {

constexpr const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
constexpr std::size_t kContourPoints = 48;

long nearest_midi(double cents) { return 69 + std::lround(cents / 100.0); }

double row_height(const PitchViewport& v) {
    const double span = v.cents_high - v.cents_low;
    return span > 0.0 ? 100.0 * v.height / span : 0.0;
}

// The found contour as it will sound: a least-squares line through it is the
// drift, what is left is the vibrato, each scaled by the note's edit.
std::vector<double> sounding_contour(const pitch::Note& n, const pitch::NoteEdit& e) {
    const std::size_t count = n.contour_cents.size();
    std::vector<double> out(count);
    if (count == 0) return out;
    const double mid = 0.5 * double(count - 1);
    double sxy = 0.0, sxx = 0.0, mean = 0.0;
    for (std::size_t k = 0; k < count; ++k) mean += double(n.contour_cents[k]) - n.median_cents;
    mean /= double(count);
    for (std::size_t k = 0; k < count; ++k) {
        const double dx = double(k) - mid;
        sxy += dx * (double(n.contour_cents[k]) - n.median_cents - mean);
        sxx += dx * dx;
    }
    const double slope = sxx > 0.0 ? sxy / sxx : 0.0;
    for (std::size_t k = 0; k < count; ++k) {
        const double d = double(n.contour_cents[k]) - n.median_cents;
        const double line = mean + slope * (double(k) - mid);
        out[k] = n.median_cents + e.shift_cents + e.drift_scale * line +
                 e.vibrato_scale * (d - line);
    }
    return out;
}

}  // namespace

PitchViewport fit_viewport(const PitchEditorModel& model, double width, double height) {
    PitchViewport v;
    v.width = width;
    v.height = height;
    v.frame_to = model.region().length;
    double lo = 0.0, hi = 0.0;
    for (std::size_t i = 0; i < model.size(); ++i) {
        const double found = model.note(i).median_cents, now = model.pitch_cents(i);
        const double a = std::min(found, now), b = std::max(found, now);
        if (i == 0 || a < lo) lo = a;
        if (i == 0 || b > hi) hi = b;
    }
    // Row boundaries sit half a semitone either side of a note's centre.
    lo = 100.0 * (std::round(lo / 100.0) - 3.0) - 50.0;
    hi = 100.0 * (std::round(hi / 100.0) + 3.0) + 50.0;
    if (hi - lo < 1200.0) {
        const double grow = std::ceil((1200.0 - (hi - lo)) / 200.0) * 100.0;
        lo -= grow;
        hi += grow;
    }
    v.cents_low = lo;
    v.cents_high = hi;
    return v;
}

double x_of_frame(const PitchViewport& v, double frame) {
    const double span = double(v.frame_to) - double(v.frame_from);
    return span > 0.0 ? (frame - double(v.frame_from)) * v.width / span : 0.0;
}

double y_of_cents(const PitchViewport& v, double cents) {
    const double span = v.cents_high - v.cents_low;
    return span > 0.0 ? (v.cents_high - cents) * v.height / span : 0.0;
}

double cents_of_dy(const PitchViewport& v, double dy) {
    return v.height > 0.0 ? -dy * (v.cents_high - v.cents_low) / v.height : 0.0;
}

std::vector<NoteBlob> note_blobs(const PitchEditorModel& model, const PitchViewport& v) {
    std::vector<NoteBlob> out;
    const double row = row_height(v);
    for (std::size_t i = 0; i < model.size(); ++i) {
        const pitch::Note& n = model.note(i);
        const pitch::NoteEdit& e = model.edit(i);
        NoteBlob b;
        b.index = int(i);
        b.x = x_of_frame(v, double(n.start));
        b.w = std::max(1.0, x_of_frame(v, double(n.end)) - b.x);
        b.h = 0.8 * row;
        b.y = y_of_cents(v, model.pitch_cents(i)) - b.h / 2.0;
        b.origin_y = y_of_cents(v, n.median_cents) - b.h / 2.0;
        b.moved = std::fabs(e.shift_cents) >= 0.5;
        b.selected = model.selected() == int(i);
        b.label = note_name(model.pitch_cents(i)) + " " + cents_off_label(model.pitch_cents(i));
        const std::vector<double> sounding = sounding_contour(n, e);
        const std::size_t step = std::max<std::size_t>(1, sounding.size() / kContourPoints);
        for (std::size_t k = 0; k < sounding.size(); k += step) {
            const double frame = double(n.start) + double(n.hop) * (double(k) + 0.5);
            b.contour.push_back(x_of_frame(v, std::min(frame, double(n.end))));
            b.contour.push_back(y_of_cents(v, sounding[k]));
        }
        out.push_back(std::move(b));
    }
    return out;
}

int hit_test(const std::vector<NoteBlob>& blobs, double x, double y) {
    int best = -1;
    double best_distance = 0.0;
    for (const NoteBlob& b : blobs) {
        const double cx = b.x + b.w / 2.0, cy = b.y + b.h / 2.0;
        const double half_w = std::max(b.w, kMinHitPixels) / 2.0;
        const double half_h = std::max(b.h, kMinHitPixels) / 2.0;
        if (std::fabs(x - cx) > half_w || std::fabs(y - cy) > half_h) continue;
        const double distance = std::hypot((x - cx) / half_w, (y - cy) / half_h);
        if (best < 0 || distance < best_distance) {
            best = b.index;
            best_distance = distance;
        }
    }
    return best;
}

std::vector<RulerRow> ruler_rows(const PitchViewport& v) {
    std::vector<RulerRow> out;
    const double row = row_height(v);
    if (row <= 0.0) return out;
    // The rows' centres are whole semitones: cents_low is a boundary.
    for (double c = v.cents_high - 50.0; c > v.cents_low; c -= 100.0) {
        const long midi = nearest_midi(c);
        const long pc = ((midi % 12) + 12) % 12;
        RulerRow r;
        r.y = y_of_cents(v, c) - row / 2.0;
        r.h = row;
        r.black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
        if (pc == 0) r.name = note_name(c);
        out.push_back(std::move(r));
    }
    return out;
}

std::string note_name(double cents) {
    const long midi = nearest_midi(cents);
    const long pc = ((midi % 12) + 12) % 12;
    const long octave = (midi - pc) / 12 - 1;
    return std::string(kNames[pc]) + std::to_string(octave);
}

std::string cents_off_label(double cents) {
    const long off = std::lround(cents - 100.0 * double(nearest_midi(cents) - 69));
    if (off == 0) return "0";
    return (off > 0 ? "+" : "") + std::to_string(off);
}

std::string key_name(int key) { return key >= 0 && key < 12 ? kNames[key] : "?"; }

std::string scale_name(pitch::Scale scale) {
    switch (scale) {
        case pitch::Scale::Chromatic: return "Chromatic";
        case pitch::Scale::Major: return "Major";
        case pitch::Scale::NaturalMinor: return "Natural minor";
        case pitch::Scale::HarmonicMinor: return "Harmonic minor";
        case pitch::Scale::MajorPentatonic: return "Major pentatonic";
        case pitch::Scale::MinorPentatonic: return "Minor pentatonic";
    }
    return "?";
}

}  // namespace sonora::ui_pitch
