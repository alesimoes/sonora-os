// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/pitch/plan.h"

#include <algorithm>
#include <initializer_list>
#include <utility>
#include <cmath>

#include "plan_pieces.h"

namespace sonora::pitch {
namespace {

// Degrees of each scale as semitones above the tonic, one bit each.
unsigned scale_mask(Scale s) {
    auto mask = [](std::initializer_list<int> degrees) {
        unsigned m = 0;
        for (const int d : degrees) m |= 1u << d;
        return m;
    };
    switch (s) {
        case Scale::Chromatic: return 0xFFFu;
        case Scale::Major: return mask({0, 2, 4, 5, 7, 9, 11});
        case Scale::NaturalMinor: return mask({0, 2, 3, 5, 7, 8, 10});
        case Scale::HarmonicMinor: return mask({0, 2, 3, 5, 7, 8, 11});
        case Scale::MajorPentatonic: return mask({0, 2, 4, 7, 9});
        case Scale::MinorPentatonic: return mask({0, 3, 5, 7, 10});
    }
    return 0xFFFu;
}

// Cents are from A4, and A is pitch class 9 with C = 0.
bool in_scale(long semitone_from_a4, int key, unsigned mask) {
    const long pitch_class = ((semitone_from_a4 + 9) % 12 + 12) % 12;
    return (mask >> ((pitch_class - key + 12) % 12)) & 1u;
}

// The scale degree nearest `cents`, in cents; the lower one on a tie.
double nearest_degree(double cents, int key, unsigned mask) {
    const long base = long(std::floor(cents / 100.0));
    double best = 0.0, best_dist = 1e30;
    for (long n = base - 12; n <= base + 13; ++n) {
        if (!in_scale(n, key, mask)) continue;
        const double dist = std::fabs(double(n) * 100.0 - cents);
        if (dist < best_dist) {
            best_dist = dist;
            best = double(n) * 100.0;
        }
    }
    return best;
}

bool finite_edit(const NoteEdit& e) {
    return std::isfinite(e.shift_cents) && std::isfinite(e.drift_scale) &&
           std::isfinite(e.vibrato_scale) && std::isfinite(e.gain_db);
}

// Pieces as spec notes: cut at the region, identity pieces left out, equal
// neighbours joined.
std::vector<api::PitchNote> build(const std::vector<Note>& notes, const std::vector<NoteEdit>& edits,
                                  uint64_t region_length, std::size_t piece_frames, bool& all_whole) {
    std::vector<api::PitchNote> out;
    all_whole = true;
    for (std::size_t i = 0; i < notes.size(); ++i) {
        const std::vector<Piece> pieces = pieces_for(notes[i], edits[i], piece_frames);
        all_whole = all_whole && pieces.size() == 1;
        for (const Piece& p : pieces) {
            api::PitchNote n;
            n.start = p.start;
            n.end = std::min<uint64_t>(p.end, region_length);
            n.cents = p.cents;
            n.formant_cents = edits[i].formant_cents;
            n.gain_db = edits[i].gain_db;
            if (n.start >= n.end) continue;
            if (n.cents == 0 && n.formant_cents == 0 && std::fabs(n.gain_db) < 0.005) continue;
            const bool joins = !out.empty() && out.back().end == n.start &&
                               out.back().cents == n.cents &&
                               out.back().formant_cents == n.formant_cents &&
                               out.back().gain_db == n.gain_db;
            if (joins) out.back().end = n.end;
            else out.push_back(n);
        }
    }
    return out;
}

}  // namespace

PitchPlan::PitchPlan(std::vector<Note> notes) : notes_(std::move(notes)), edits_(notes_.size()) {}

bool PitchPlan::snap_to_scale(int key, Scale scale, double amount, std::string& why) {
    if (key < 0 || key > 11) {
        why = "pitch: the key is a pitch class from 0 (C) to 11 (B)";
        return false;
    }
    if (!(amount >= 0.0 && amount <= 100.0)) {
        why = "pitch: snap amount is 0 to 100 percent";
        return false;
    }
    const unsigned mask = scale_mask(scale);
    for (std::size_t i = 0; i < notes_.size(); ++i) {
        const double now = notes_[i].median_cents + edits_[i].shift_cents;
        edits_[i].shift_cents += amount / 100.0 * (nearest_degree(now, key, mask) - now);
    }
    return true;
}

bool PitchPlan::to_spec(uint64_t region_length, api::PitchEditSpec& spec, std::string& why) const {
    for (const NoteEdit& e : edits_) {
        if (!finite_edit(e)) {
            why = "pitch: an edit holds a value that is not a number";
            return false;
        }
    }
    // 50 ms pieces first; doubled while the 64-note limit is passed.
    std::size_t piece_frames = 1;
    if (!notes_.empty() && notes_[0].hop > 0) {
        piece_frames = std::max<std::size_t>(1, std::size_t(std::lround(0.05 * notes_[0].rate / notes_[0].hop)));
    }
    api::PitchEditSpec out;
    for (int attempt = 0; attempt < 16; ++attempt) {
        bool all_whole = true;
        out.notes = build(notes_, edits_, region_length, piece_frames, all_whole);
        if (out.notes.size() <= api::kMaxPitchNotes) break;
        if (all_whole) {
            why = "pitch: " + std::to_string(out.notes.size()) + " notes to change, at most " +
                  std::to_string(api::kMaxPitchNotes) + "; edit fewer notes at a time";
            return false;
        }
        piece_frames *= 2;
    }
    if (out.notes.empty()) {
        why = "pitch: nothing to change";
        return false;
    }
    if (!api::validate_pitch_spec(out, why)) return false;
    spec = std::move(out);
    return true;
}

}  // namespace sonora::pitch
