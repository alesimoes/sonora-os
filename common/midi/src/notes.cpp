// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/midi/notes.h"

#include <algorithm>
#include <deque>
#include <iterator>
#include <numeric>
#include <optional>

namespace sonora::midi {

namespace {

std::vector<std::size_t> tick_order(const EventList& events) {
    std::vector<std::size_t> order(events.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return events[a].tick < events[b].tick;
    });
    return order;
}

}  // namespace

void sort_notes(NoteList& notes) {
    std::stable_sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) {
        if (a.start != b.start) return a.start < b.start;
        if (a.channel != b.channel) return a.channel < b.channel;
        if (a.pitch != b.pitch) return a.pitch < b.pitch;
        return a.end() < b.end();
    });
}

NoteExtraction extract_notes(const EventList& events) {
    NoteExtraction out;
    // Sounding notes per channel and pitch, oldest first, as indices into
    // out.notes.
    std::vector<std::deque<std::size_t>> sounding(16 * 128);
    Tick last_tick = 0;

    for (const std::size_t i : tick_order(events)) {
        const Event& e = events[i];
        last_tick = std::max(last_tick, e.tick);
        if (e.is_note_on()) {
            Note n;
            n.start = e.tick;
            n.channel = e.channel();
            n.pitch = e.note();
            n.velocity = e.velocity();
            sounding[n.channel * 128u + n.pitch].push_back(out.notes.size());
            out.notes.push_back(n);
        } else if (e.is_note_off()) {
            auto& queue = sounding[e.channel() * 128u + e.note()];
            if (queue.empty()) {
                ++out.unmatched_note_offs;
                continue;
            }
            Note& n = out.notes[queue.front()];
            queue.pop_front();
            n.length = e.tick - n.start;
            // A note-on of velocity 0 carries no release velocity.
            n.release_velocity = (e.status & 0xF0) == status::note_off
                                     ? e.velocity()
                                     : default_release_velocity;
        } else {
            out.other.push_back(e);
        }
    }

    for (const auto& queue : sounding) {
        for (const std::size_t idx : queue) {
            out.notes[idx].length = last_tick - out.notes[idx].start;
            ++out.unterminated_notes;
        }
    }
    sort_by_tick(out.other);
    sort_notes(out.notes);
    return out;
}

EventList notes_to_events(const NoteList& input) {
    NoteList notes = input;
    sort_notes(notes);

    // At one tick: offs of notes with length, then ons, then offs of
    // zero-length notes. Within a class, canonical note order.
    struct Keyed {
        Tick tick;
        int cls;
        std::size_t seq;
        Event event;
    };
    std::vector<Keyed> keyed;
    keyed.reserve(notes.size() * 2);
    for (std::size_t i = 0; i < notes.size(); ++i) {
        const Note& n = notes[i];
        const Tick end = n.start + std::max<Tick>(n.length, 0);
        keyed.push_back({n.start, 1, i, note_on(n.start, n.channel, n.pitch,
                                                std::max<std::uint8_t>(n.velocity, 1))});
        keyed.push_back({end, end == n.start ? 2 : 0, i,
                         note_off(end, n.channel, n.pitch, n.release_velocity)});
    }
    std::stable_sort(keyed.begin(), keyed.end(), [](const Keyed& a, const Keyed& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        if (a.cls != b.cls) return a.cls < b.cls;
        return a.seq < b.seq;
    });

    EventList out;
    out.reserve(keyed.size());
    for (Keyed& k : keyed) out.push_back(std::move(k.event));
    return out;
}

EventList merge_events(const EventList& a, const EventList& b) {
    EventList sa;
    EventList sb;
    std::optional<Tick> eot;
    for (const Event& e : a) {
        if (e.is_end_of_track()) {
            eot = std::max(eot.value_or(e.tick), e.tick);
        } else {
            sa.push_back(e);
        }
    }
    for (const Event& e : b) {
        if (e.is_end_of_track()) {
            eot = std::max(eot.value_or(e.tick), e.tick);
        } else {
            sb.push_back(e);
        }
    }
    sort_by_tick(sa);
    sort_by_tick(sb);

    EventList out;
    out.reserve(sa.size() + sb.size() + 1);
    // std::merge takes from the first range on ties, which is the a-before-b rule.
    std::merge(sa.begin(), sa.end(), sb.begin(), sb.end(), std::back_inserter(out),
               [](const Event& x, const Event& y) { return x.tick < y.tick; });
    if (eot) {
        const Tick last = out.empty() ? *eot : std::max(*eot, out.back().tick);
        out.push_back(end_of_track(last));
    }
    return out;
}

}  // namespace sonora::midi
