// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Applier's own checks and edits, which every family's
// operations call (apply_detail.h).
#include "apply_detail.h"

namespace sonora::model::apply_detail {

std::string Applier::path_taken(const std::string& path) const {
    for (const Source& s : before_.sources->sources) {
        if (s.path == path) return "path \"" + path + "\" is already source " + str(s.id);
    }
    for (const MidiSource& s : before_.sources->midi_sources) {
        if (s.path == path) {
            return "path \"" + path + "\" is already MIDI source " + str(s.id);
        }
    }
    return {};
}

bool Applier::no_audio(Id track) {
    return fail("track " + str(track) + " is a MIDI track; it has no audio to insert a"
                " plug-in on");
}

bool Applier::not_midi(Id playlist, const char* instead) {
    Located at;
    if (!locate_midi_playlist(*before_.edit, playlist, at)) return true;
    return fail("playlist " + str(playlist) + " is a MIDI playlist; " + instead);
}

bool Applier::timebase_matches(Id playlist, Timebase timebase) {
    Located at;
    const bool midi = locate_midi_playlist(*before_.edit, playlist, at);
    if (!midi && !locate_playlist(*before_.edit, playlist, at)) {
        return fail("playlist " + str(playlist) + " does not exist");
    }
    if (midi == (timebase == Timebase::Ticks)) return true;
    return fail("playlist " + str(playlist) +
                (midi ? " is a MIDI playlist, in ticks, and this operation's position is in"
                        " frames"
                      : " is an audio playlist, in frames, and this operation's position is"
                        " in ticks"));
}

bool Applier::value_ok(const MidiLists& one, const char* list, std::size_t i) {
    std::string where;
    const std::string why = detail::sequence_problem(one, where);
    if (why.empty()) return true;
    return fail(std::string(list) + "[" + str(static_cast<std::uint64_t>(i)) + "]: " + why);
}

bool Applier::midi_overwrite(MidiPlaylist& p, Ticks a, Ticks b, Id split_id, bool& used) {
    std::vector<MidiRegion> out;
    out.reserve(p.regions.size() + 1);
    for (MidiRegion& r : p.regions) {
        const Ticks s = r.position;
        const Ticks e = r.end();
        if (e <= a || s >= b) {
            out.push_back(std::move(r));
        } else if (s >= a && e <= b) {
            // Covered: removed. Its sequence lives on in the undo steps.
        } else if (s < a && e > b) {
            if (split_id == 0) {
                return fail("region " + str(r.id) +
                            " spans the whole range and must be split, but the"
                            " operation carries no split id");
            }
            MidiRegion right = r;
            right.id = split_id;
            right.position = b;
            right.offset += b - s;
            right.length = e - b;
            r.length = a - s;
            out.push_back(std::move(r));
            out.push_back(std::move(right));
            used = true;
        } else if (s < a) {
            r.length = a - s;
            out.push_back(std::move(r));
        } else {
            r.offset += b - s;
            r.length = e - b;
            r.position = b;
            out.push_back(std::move(r));
        }
    }
    p.regions = std::move(out);
    return true;
}

bool Applier::move_midi(const MoveRegion& op) {
    Id used_id = 0;
    const bool ok = edit_midi_playlist(op.playlist, [&](MidiPlaylist& p) {
        const std::size_t i = midi_region_index(p, op.region);
        if (i == kNone) return no_region(op.region, op.playlist);
        MidiRegion r = p.regions[i];
        p.regions.erase(p.regions.begin() + static_cast<std::ptrdiff_t>(i));
        if (op.position < 0 || op.position > kMaxInteger - r.length) {
            return fail("position " + str(op.position) + " is outside the timeline");
        }
        r.position = op.position;
        bool used = false;
        if (!midi_overwrite(p, r.position, r.end(), op.split_right_id, used)) return false;
        if (used) {
            if (!fresh(op.split_right_id)) return false;
            used_id = op.split_right_id;
        }
        insert_midi_region(p, std::move(r));
        return true;
    });
    if (ok && used_id != 0) after_.next_id = used_id + 1;
    return ok;
}

bool Applier::trim_midi(const TrimRegion& op) {
    return edit_midi_playlist(op.playlist, [&](MidiPlaylist& p) {
        const std::size_t i = midi_region_index(p, op.region);
        if (i == kNone) return no_region(op.region, op.playlist);
        MidiRegion r = p.regions[i];
        p.regions.erase(p.regions.begin() + static_cast<std::ptrdiff_t>(i));
        const Ticks start = r.position;
        const Ticks end = r.end();
        if (op.edge == Edge::Start) {
            if (op.to >= end) return fail("trimming the start to or past the end");
            if (op.to < 0) return fail("trimming the start before timeline tick 0");
            // The notes stay where they are on the timeline.
            if (r.offset + (op.to - start) < 0) {
                return fail("region " + str(r.id) + " shows its sequence from data tick " +
                            str(r.offset) + "; its start cannot move back past tick 0");
            }
            r.offset += op.to - start;
            r.position = op.to;
            r.length = end - op.to;
        } else {
            if (op.to <= start) return fail("trimming the end to or before the start");
            if (op.to > kMaxInteger) return fail("trimming past the end of the timeline");
            r.length = op.to - start;
        }
        bool used = false;
        if (!midi_overwrite(p, r.position, r.end(), 0, used)) return false;
        insert_midi_region(p, std::move(r));
        return true;
    });
}

bool Applier::split_midi(const SplitRegion& op) {
    const bool ok = edit_midi_playlist(op.playlist, [&](MidiPlaylist& p) {
        const std::size_t i = midi_region_index(p, op.region);
        if (i == kNone) return no_region(op.region, op.playlist);
        const MidiRegion& r = p.regions[i];
        if (op.at <= r.position || op.at >= r.end()) {
            return fail("split point " + str(op.at) + " is not inside region " +
                        str(op.region));
        }
        if (!fresh(op.new_right_id)) return false;
        bool used = false;
        return midi_overwrite(p, op.at, op.at, op.new_right_id, used);
    });
    if (ok) after_.next_id = op.new_right_id + 1;
    return ok;
}

bool Applier::no_insert(Id insert) {
    return fail("insert " + str(insert) + " is not on any track");
}

std::size_t Applier::bus_index(Id bus) const {
    const auto& buses = before_.edit->buses;
    for (std::size_t b = 0; b < buses.size(); ++b) {
        if (buses[b].id == bus) return b;
    }
    return kNone;
}

std::string Applier::bus_name(Id bus) const {
    const std::size_t b = bus_index(bus);
    return b == kNone ? str(bus) : "\"" + before_.edit->buses[b].name + "\"";
}

bool Applier::has_send(const std::vector<Send>& sends, Id send) {
    for (const Send& sd : sends) {
        if (sd.id == send) return true;
    }
    return false;
}

bool Applier::send_exists(Id send) const {
    for (const auto& t : before_.edit->tracks) {
        if (has_send(t->sends, send)) return true;
    }
    for (const Bus& b : before_.edit->buses) {
        if (has_send(b.sends, send)) return true;
    }
    return false;
}

bool Applier::no_send(Id send) {
    return fail("send " + str(send) + " is not on any track or aux bus");
}

void Applier::set_send_mix(MixState& mix, const SendMix& m) {
    for (SendMix& existing : mix.sends) {
        if (existing.send == m.send) {
            existing = m;
            return;
        }
    }
    mix.sends.push_back(m);
}

void Applier::set_bus_mix(MixState& mix, const BusMix& m) {
    for (BusMix& existing : mix.buses) {
        if (existing.bus == m.bus) {
            existing = m;
            return;
        }
    }
    mix.buses.push_back(m);
}

bool Applier::free_slot(const TrackEdit& t, std::uint8_t slot, Id moving) {
    if (slot >= kInsertSlots) {
        return fail("slot " + str(static_cast<std::uint64_t>(slot)) +
                    " is not one of the " + str(static_cast<std::uint64_t>(kInsertSlots)) +
                    " insert slots (A to J)");
    }
    const Insert* there = in_slot(t.inserts, slot);
    if (there != nullptr && there->id != moving) {
        return fail("slot " + slot_name(slot) + " on track " + str(t.id) +
                    " already holds insert " + str(there->id) + " (" +
                    there->plugin.name + ")");
    }
    return true;
}

void Applier::set_insert_mix(MixState& mix, InsertMix m) {
    for (InsertMix& existing : mix.inserts) {
        if (existing.insert == m.insert) {
            existing = std::move(m);
            return;
        }
    }
    mix.inserts.push_back(std::move(m));
}

bool Applier::fail(std::string msg) {
    error_ = std::move(msg);
    return false;
}

bool Applier::no_region(Id region, Id playlist) {
    return fail("region " + str(region) + " is not on playlist " + str(playlist));
}

bool Applier::fresh(Id id) {
    if (id == 0) return fail("a new object needs an id; 0 is not one");
    if (id < before_.next_id) {
        return fail("id " + str(id) + " is already taken (new ids start at " +
                    str(before_.next_id) + ")");
    }
    if (id > static_cast<Id>(kMaxInteger)) return fail("id " + str(id) + " is out of range");
    return true;
}

void Applier::set_mix(MixState& mix, const TrackMix& m) {
    for (TrackMix& existing : mix.tracks) {
        if (existing.track == m.track) {
            existing = m;
            return;
        }
    }
    mix.tracks.push_back(m);
}

bool Applier::place(Id playlist, const Region& region, Id split_id,
           const std::vector<Crossfade>& crossfades) {
    if (!fresh(region.id)) return false;
    Id top = region.id;
    const bool ok = edit_playlist(playlist, [&](Playlist& p, std::uint16_t ch) {
        const std::string why = detail::region_problem(region, *before_.sources, ch);
        if (!why.empty()) return fail("region " + str(region.id) + ": " + why);
        bool used = false;
        if (!overwrite(p, region.position, region.end(), split_id, used)) return false;
        if (used) {
            if (split_id == region.id) {
                return fail("the split id must differ from the region's id");
            }
            if (!fresh(split_id)) return false;
            top = std::max(top, split_id);
        }
        insert_region(p, region);
        drop_broken_crossfades(p);
        for (const Crossfade& x : crossfades) {
            if (x.left != region.id && x.right != region.id) {
                return fail("a take's crossfade must join the take (region " +
                            str(region.id) + ")");
            }
            const std::string bad = crossfade_problem(x, p);
            if (!bad.empty()) return fail("crossfade: " + bad);
            add_crossfade(p, x);
        }
        // A punch near a region's start must not be refused for that
        // region's fade-in: losing a take is worse than a shorter fade.
        fit_fades(p);
        return true;
    });
    if (ok) after_.next_id = top + 1;
    return ok;
}

bool Applier::refuse_playlist(Id playlist, const std::string& why, const std::string& where) {
    return fail("playlist " + str(playlist) + (where.empty() ? "" : "." + where) + ": " +
                why);
}

bool Applier::overwrite(Playlist& p, Frames a, Frames b, Id split_id, bool& used) {
#if SONORA_MODEL_AUDIT
    if (reference_) return reference::overwrite(p, a, b, split_id, used, error_);
#endif
    return detail::overwrite_range(p, a, b, split_id, used, error_, touched_);
}

void Applier::insert_region(Playlist& p, Region r) {
#if SONORA_MODEL_AUDIT
    if (reference_) return reference::insert_sorted(p, std::move(r));
#endif
    detail::insert_region(p, std::move(r), touched_);
}

void Applier::erase_region(Playlist& p, std::size_t i) {
#if SONORA_MODEL_AUDIT
    if (reference_) {
        p.regions.erase(p.regions.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
#endif
    detail::erase_region(p, i, touched_);
}

void Applier::mark_crossfade(const Playlist& p, const Crossfade& x) {
    const std::size_t l = detail::find_index(p, x.left, touched_);
    const std::size_t r = detail::find_index(p, x.right, touched_);
    if (l != detail::kNoIndex && r != detail::kNoIndex) {
        mark(std::min(l, r), std::max(l, r));
    }
}

void Applier::drop_broken_crossfades(Playlist& p) {
#if SONORA_MODEL_AUDIT
    if (reference_) return reference::drop_broken_crossfades(p, *before_.sources);
#endif
    detail::drop_broken_crossfades_near(p, *before_.sources, touched_);
    detail::fit_fades_near(p, touched_);
}

void Applier::fit_fades(Playlist& p) {
#if SONORA_MODEL_AUDIT
    if (reference_) {
        detail::fit_fades_to_crossfades(p);
        return;
    }
#endif
    detail::fit_fades_near(p, touched_);
}

std::string Applier::crossfade_problem(const Crossfade& x, const Playlist& p) const {
    const std::size_t l = detail::find_index(p, x.left, touched_);
    const std::size_t r = detail::find_index(p, x.right, touched_);
    return detail::crossfade_problem(x, l == detail::kNoIndex ? nullptr : &p.regions[l],
                                     r == detail::kNoIndex ? nullptr : &p.regions[r],
                                     *before_.sources);
}

void Applier::add_crossfade(Playlist& p, const Crossfade& x) {
#if SONORA_MODEL_AUDIT
    if (reference_) {
        p.crossfades.push_back(x);
        return;
    }
#endif
    detail::insert_crossfade(p, x, detail::find_index(p, x.left, touched_));
    mark_crossfade(p, x);
}

#if SONORA_MODEL_AUDIT

void Applier::audit_failed(const std::string& what) {
    std::fprintf(stderr, "common/model audit: %s\n", what.c_str());
    std::abort();
}

void Applier::audit_verdict(const Playlist& p, std::uint16_t channels, const std::string& why,
                   const std::string& where) const {
    std::string full_where;
    const std::string full =
        detail::playlist_problem(p, *before_.sources, channels, full_where);
    if (full != why || full_where != where) {
        audit_failed("the local check says \"" + where + ": " + why +
                     "\", playlist_problem says \"" + full_where + ": " + full + "\"");
    }
}

void Applier::audit_against_reference(const Audit& a, bool ok, const Playlist& p) const {
    if (a.ok != ok) {
        audit_failed(std::string("the whole-playlist edit ") +
                     (a.ok ? "succeeds" : "fails (" + a.error + ")") +
                     ", the range-local one " + (ok ? "succeeds" : "fails (" + error_ + ")"));
    }
    if (!ok && a.error != error_) {
        audit_failed("refused as \"" + a.error + "\" by the whole-playlist edit and as \"" +
                     error_ + "\" by the range-local one");
    }
    if (ok && !(a.playlist == p)) {
        audit_failed("the whole-playlist and range-local edits of playlist " + str(p.id) +
                     " differ");
    }
}
#endif


}  // namespace sonora::model::apply_detail
