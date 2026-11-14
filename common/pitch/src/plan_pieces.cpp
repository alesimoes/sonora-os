// SPDX-License-Identifier: GPL-3.0-or-later
#include "plan_pieces.h"

#include <algorithm>
#include <cmath>

namespace sonora::pitch {
namespace {

// Slow part of the contour: two passes of a 200 ms box. One pass leaks about
// 15% of a 6 Hz vibrato, two leave about 2%.
std::vector<double> trend_of(const std::vector<double>& d, std::size_t hop, uint32_t rate) {
    const long half = std::max(1L, std::lround(0.1 * double(rate) / double(hop)));
    std::vector<double> a = d, b(d.size());
    for (int pass = 0; pass < 2; ++pass) {
        for (long i = 0; i < long(a.size()); ++i) {
            const long lo = std::max(0L, i - half), hi = std::min(long(a.size()) - 1, i + half);
            double sum = 0.0;
            for (long j = lo; j <= hi; ++j) sum += a[std::size_t(j)];
            b[std::size_t(i)] = sum / double(hi - lo + 1);
        }
        a.swap(b);
    }
    return a;
}

// Per frame, what the shift must add to turn the note's own drift and vibrato
// into the scaled ones. Empty when both scales are 1.
std::vector<double> corrections(const Note& note, const NoteEdit& edit, uint32_t rate) {
    if (edit.drift_scale == 1.0 && edit.vibrato_scale == 1.0) return {};
    std::vector<double> d(note.contour_cents.size());
    for (std::size_t i = 0; i < d.size(); ++i) d[i] = double(note.contour_cents[i]) - note.median_cents;
    const std::vector<double> trend = trend_of(d, note.hop, rate);
    std::vector<double> out(d.size());
    for (std::size_t i = 0; i < d.size(); ++i) {
        out[i] = (edit.drift_scale - 1.0) * trend[i] + (edit.vibrato_scale - 1.0) * (d[i] - trend[i]);
    }
    return out;
}

int32_t whole_cents(double cents) {
    return int32_t(std::lround(std::clamp(cents, double(api::kPitchCentsMin), double(api::kPitchCentsMax))));
}

}  // namespace

std::vector<Piece> pieces_for(const Note& note, const NoteEdit& edit, std::size_t piece_frames) {
    const std::vector<double> corr = corrections(note, edit, note.rate);
    if (corr.empty() || piece_frames >= corr.size()) {
        double mean = 0.0;
        for (const double c : corr) mean += c;
        if (!corr.empty()) mean /= double(corr.size());
        return {Piece{note.start, note.end, whole_cents(edit.shift_cents + mean)}};
    }
    std::vector<Piece> pieces;
    for (std::size_t from = 0; from < corr.size(); from += piece_frames) {
        const std::size_t to = std::min(corr.size(), from + piece_frames);
        double mean = 0.0;
        for (std::size_t i = from; i < to; ++i) mean += corr[i];
        mean /= double(to - from);
        Piece p;
        p.start = from == 0 ? note.start : note.start + from * note.hop;
        p.end = to == corr.size() ? note.end : note.start + to * note.hop;
        p.cents = whole_cents(edit.shift_cents + mean);
        pieces.push_back(p);
    }
    return pieces;
}

}  // namespace sonora::pitch
