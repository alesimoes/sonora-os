// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/midi/edit.h"

#include <algorithm>

namespace sonora::midi {

namespace {

Tick floor_div(Tick a, Tick b) {
    Tick q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

// a * num / den rounded half away from zero; den > 0.
Tick scale_round(Tick a, Tick num, Tick den) {
    const Tick p = a * num;
    return p >= 0 ? (p + den / 2) / den : -((-p + den / 2) / den);
}

class Grid {
public:
    explicit Grid(const QuantizeSettings& s)
        : step_(s.grid),
          origin_(s.origin),
          swing_(scale_round(s.grid, std::clamp(s.swing_percent, 0, 300), 300)) {}

    Tick nearest(Tick t) const {
        const Tick base = origin_ + floor_div(t - origin_, 2 * step_) * 2 * step_;
        const Tick candidates[3] = {base, base + step_ + swing_, base + 2 * step_};
        Tick best = candidates[0];
        for (const Tick c : candidates) {
            const Tick dc = c > t ? c - t : t - c;
            const Tick db = best > t ? best - t : t - best;
            if (dc < db) best = c;
        }
        return best;
    }

private:
    Tick step_;
    Tick origin_;
    Tick swing_;
};

std::uint8_t clamp_velocity(Tick v) {
    return static_cast<std::uint8_t>(std::clamp<Tick>(v, 1, 127));
}

NoteList sorted(NoteList notes) {
    sort_notes(notes);
    return notes;
}

}  // namespace

NoteList quantize(const NoteList& notes, const QuantizeSettings& settings) {
    if (settings.grid <= 0) return sorted(notes);
    const Grid grid(settings);
    const Tick strength = std::clamp(settings.strength_percent, 0, 100);
    auto pull = [&](Tick t) { return t + scale_round(grid.nearest(t) - t, strength, 100); };

    NoteList out = notes;
    for (Note& n : out) {
        const Tick start = std::max<Tick>(pull(n.start), 0);
        if (settings.mode == QuantizeMode::NoteOnAndLength) {
            Tick end = pull(n.end());
            if (end <= start) end = start + settings.grid;
            n.length = end - start;
        }
        n.start = start;
    }
    return sorted(std::move(out));
}

NoteList transpose(const NoteList& notes, int semitones) {
    NoteList out = notes;
    for (Note& n : out) {
        n.pitch = static_cast<std::uint8_t>(std::clamp(n.pitch + semitones, 0, 127));
    }
    return sorted(std::move(out));
}

NoteList set_velocity(const NoteList& notes, std::uint8_t velocity) {
    NoteList out = notes;
    for (Note& n : out) n.velocity = clamp_velocity(velocity);
    return sorted(std::move(out));
}

NoteList scale_velocity(const NoteList& notes, int percent, int offset) {
    NoteList out = notes;
    for (Note& n : out) {
        n.velocity = clamp_velocity(scale_round(n.velocity, percent, 100) + offset);
    }
    return sorted(std::move(out));
}

NoteList legato(const NoteList& notes, Tick gap) {
    NoteList out = sorted(notes);
    // out is sorted by start, so the next later start is found walking back.
    Tick next_start = 0;
    bool have_next = false;
    for (std::size_t i = out.size(); i-- > 0;) {
        Note& n = out[i];
        if (i + 1 < out.size() && out[i + 1].start > n.start) {
            next_start = out[i + 1].start;
            have_next = true;
        }
        if (have_next) n.length = std::max<Tick>(next_start - n.start - gap, 1);
    }
    return sorted(std::move(out));
}

NoteList split_notes(const NoteList& notes, Tick position) {
    NoteList out;
    out.reserve(notes.size() + notes.size() / 4);
    for (const Note& n : notes) {
        if (n.start < position && position < n.end()) {
            Note head = n;
            head.length = position - n.start;
            Note tail = n;
            tail.start = position;
            tail.length = n.end() - position;
            out.push_back(head);
            out.push_back(tail);
        } else {
            out.push_back(n);
        }
    }
    return sorted(std::move(out));
}

NoteList merge_notes(const NoteList& a, const NoteList& b) {
    NoteList out = a;
    out.insert(out.end(), b.begin(), b.end());
    return sorted(std::move(out));
}

}  // namespace sonora::midi
