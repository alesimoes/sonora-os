// SPDX-License-Identifier: GPL-3.0-or-later
//
// What apply()'s translation units share: the lookups every operation
// uses and the Applier, whose operator() for each operation lives in the
// file of its family (apply_<family>.cpp, ADR-0059).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "checks.h"
#include "local_edit.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/operations.h"

namespace sonora::model::apply_detail {

constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();

inline std::string str(std::uint64_t n) { return std::to_string(n); }
inline std::string str(std::int64_t n) { return std::to_string(n); }

struct Located {
    std::size_t track = kNone;
    std::size_t playlist = kNone;
};

bool locate_playlist(const EditState& e, Id playlist, Located& at);

bool locate_midi_playlist(const EditState& e, Id playlist, Located& at);

std::size_t midi_region_index(const MidiPlaylist& p, Id region);

void insert_midi_region(MidiPlaylist& p, MidiRegion r);

std::size_t locate_track(const EditState& e, Id track);

// A copy of `p` named `name`, its id `next` and its regions' the ids after
// it, in position order; `next` is left past the last. DuplicateTrack and
// DuplicatePlaylist number their copies this way.
std::shared_ptr<Playlist> copy_playlist(const Playlist& p, const std::string& name, Id& next);

std::shared_ptr<MidiPlaylist> copy_playlist(const MidiPlaylist& p, const std::string& name,
                                            Id& next);

struct InsertAt {
    std::size_t track = kNone;
    std::size_t index = kNone;
};

bool locate_insert(const EditState& e, Id insert, InsertAt& at);

void insert_by_slot(std::vector<Insert>& v, Insert in);

const Insert* in_slot(const std::vector<Insert>& v, std::uint8_t slot);

// Unkeys every insert keyed by `bus` (ADR-0058 �1): the removal that takes
// the bus takes its keys in the same step, so one undo restores both.
void clear_keys(EditState& edit, Id bus);

std::string slot_name(std::uint8_t slot);

std::size_t region_index(const Playlist& p, Id region);

using detail::clamp_fades;

#if SONORA_MODEL_AUDIT
// The whole-playlist operators the range-local ones replaced (local_edit.h),
// kept verbatim in audited builds: every edit is run through both, and the
// results must be equal.
namespace reference {

void insert_sorted(Playlist& p, Region r);

// Clears [a, b) on the playlist (timeline-model.md, "Overwrite"): a region
// inside goes, one straddling a is cut to end at a, one straddling b is cut
// to start at b, and one spanning both is split, its right part taking
// `split_id`. A cut edge loses its fade, which belonged to the old edge.
bool overwrite(Playlist& p, Frames a, Frames b, Id split_id, bool& used_split,
               std::string& error);

// After an edit moved region edges, a crossfade stays only if it would
// still be accepted as new: both regions there, butting, with the handle
// it needs, and not meeting another crossfade inside a region. The same
// rule decides for every crossfade, which is what "removed or kept by the
// same rule" asks of the overwrite.
void drop_broken_crossfades(Playlist& p, const SourcePool& pool);

}  // namespace reference
#endif

class Applier {
public:
    Applier(const Snapshot& before, Snapshot& after, std::string& error, bool in_fold)
        : before_(before), after_(after), error_(error), in_fold_(in_fold) {}

    // apply() folds a compound itself; the visit never reaches here.
    bool operator()(const Compound&) { return fail("a compound inside a compound"); }

    // --- not undoable ---

    bool operator()(const AddSource& op);
    bool operator()(const SetMix& op);
    bool operator()(const SetMonitorBus& op);
    bool operator()(const SetSettings& op);

    // --- undoable ---

    bool operator()(const AddTrack& op);
    bool operator()(const PlaceRegion& op);
    bool operator()(const RecordTake& op);
    // The leaves it is made of, applied in turn as a compound's members
    // are: each sees what the one before it made, and their own checks
    // (fresh ids, the overwrite, the crossfades' handles) are the whole
    // validation, so the result is what those leaves in a compound give.
    bool operator()(const RecordLoopTake& op);
    bool operator()(const MoveRegion& op);
    bool operator()(const TrimRegion& op);
    bool operator()(const SplitRegion& op);
    bool operator()(const RemoveRegion& op);
    bool operator()(const ShiftRegions& op);
    bool operator()(const SetFade& op);
    bool operator()(const SetCrossfade& op);
    bool operator()(const RemoveCrossfade& op);
    bool operator()(const AddPlaylist& op);
    bool operator()(const SetActivePlaylist& op);
    bool operator()(const DuplicatePlaylist& op);
    bool operator()(const SetPlaylistName& op);
    bool operator()(const RemovePlaylist& op);
    bool operator()(const SetTrackName& op);
    bool operator()(const SetTrackColour& op);
    bool operator()(const SetRegionName& op);
    bool operator()(const SetGroup& op);
    bool operator()(const RemoveGroup& op);
    bool operator()(const SetRegionColour& op);
    bool operator()(const SetRegionGain& op);
    bool operator()(const SetClipGroup& op);
    bool operator()(const SetRegionSources& op);
    bool operator()(const ReorderTracks& op);
    bool operator()(const RemoveTrack& op);

    template <typename T, typename Op>
    bool set_members(std::vector<T>& list, Id id, const Op& op) {
        const auto at = std::find_if(list.begin(), list.end(), [&](const T& x) { return x.id == id; });
        if (at == list.end() && !fresh(id)) return false;
        T item;
        item.id = id;
        item.name = op.name;
        item.members = op.members;
        if (at == list.end()) {
            list.push_back(std::move(item));
        } else {
            *at = std::move(item);
        }
        return true;
    }

    bool operator()(const SetVca& op);
    bool operator()(const RemoveVca& op);
    bool operator()(const SetVcaMix& op);
    bool operator()(const SetFolder& op);
    bool operator()(const RemoveFolder& op);
    bool operator()(const DuplicateTrack& op);
    bool operator()(const AddMarker& op);
    bool operator()(const MoveMarker& op);
    bool operator()(const RemoveMarker& op);

    // --- inserts: structure, undoable (O1's default) ---

    bool operator()(const AddInsert& op);
    bool operator()(const RemoveInsert& op);
    bool operator()(const MoveInsert& op);
    bool operator()(const SetInsertActive& op);
    bool operator()(const ReplaceInsert& op);

    // --- inserts: values, mix state ---

    bool operator()(const SetInsertBypass& op);
    bool operator()(const SetInsertParams& op);
    bool operator()(const SetInsertState& op);

    // --- sends and aux buses (ADR-0022): structure undoable, values not ---

    bool operator()(const AddBus& op);
    bool operator()(const RemoveBus& op);
    bool operator()(const AddSend& op);
    bool operator()(const RemoveSend& op);
    // Only a send that is on a strip: values left by a removal are kept for
    // undo, not edited.
    bool operator()(const SetSendMix& op);
    bool operator()(const SetBusMix& op);

    // --- output routing (ADR-0024): structure, undoable ---

    bool operator()(const SetOutput& op);

    // --- hardware output paths (ADR-0060): undoable ---

    bool operator()(const SetBusPath& op);

    // --- side-chain keys (ADR-0058): structure, undoable ---

    bool operator()(const SetInsertKey& op);

    // --- automation (ADR-0027 §1): undoable ---

    // `op.track` names the strip (ADR-0027 §9): a track, an aux bus, or the
    // Master.
    bool operator()(const EditAutomation& op);

    // --- MIDI (midi.md §3.1) ---

    bool operator()(const AddMidiSource& op);
    bool operator()(const PlaceMidiRegion& op);
    bool operator()(const EditMidi& op);
    bool operator()(const SetMidiRouting& op);

private:
    // "path ... is already source N", or empty: audio and MIDI sources
    // share the folder, and a path names one file.
    std::string path_taken(const std::string& path) const;

    bool no_audio(Id track);

    // For the operations only an audio playlist has.
    bool not_midi(Id playlist, const char* instead);

    // A26: frames sent to a tick playlist, or the reverse, are refused by
    // name rather than taken for the other unit.
    bool timebase_matches(Id playlist, Timebase timebase);

    bool value_ok(const MidiLists& one, const char* list, std::size_t i);

    static const midi::Note& expected(const NoteRemoval& r) { return r.note; }
    static const MidiEvent& expected(const EventRemoval& r) { return r.event; }
    static const MidiSysEx& expected(const SysExRemoval& r) { return r.sysex; }

    static Ticks tick_of(const midi::Note& n) { return n.start; }
    static Ticks tick_of(const MidiEvent& e) { return e.tick; }
    static Ticks tick_of(const MidiSysEx& x) { return x.tick; }

    // What one EditMidi does to one chunk: positions in it to remove, in
    // increasing order, and what lands in it.
    struct Touched {
        std::vector<std::size_t> notes;
        std::vector<std::size_t> events;
        std::vector<std::size_t> sysex;
        MidiLists add;
    };

    // Each removal names its element by its index in the whole sequence,
    // and must hold the value the removal expects: an index alone would
    // remove whatever a concurrent edit moved there. Recorded against the
    // chunk that holds it.
    template <typename T, typename R>
    bool removals(const MidiSequence& seq, const MidiSequence::List<T>& from,
                  const std::vector<R>& remove, const char* list, const char* what,
                  std::map<std::int64_t, Touched>& touched,
                  std::vector<std::size_t> Touched::*into) {
        for (std::size_t k = 0; k < remove.size(); ++k) {
            const std::string at =
                std::string(list) + "[" + str(static_cast<std::uint64_t>(k)) + "]: ";
            const std::uint32_t i = remove[k].index;
            if (i >= from.size() || (k > 0 && i <= remove[k - 1].index)) {
                return fail(at + "indices must be increasing and below " +
                            str(static_cast<std::uint64_t>(from.size())));
            }
            const T& v = from[i];
            if (!(v == expected(remove[k]))) {
                return fail(at + what + " " + str(static_cast<std::uint64_t>(i)) +
                            " is not the " + what +
                            " this edit removes; the region changed under it");
            }
            const std::int64_t c = midi_chunk_of(tick_of(v));
            const std::vector<T>& in = detail::ChunkList<T>::of(*seq.chunk_at(c));
            (touched[c].*into).push_back(static_cast<std::size_t>(&v - in.data()));
        }
        return true;
    }

    // `was` without the positions in `drop` (increasing), then `add`.
    template <typename T>
    static void rebuilt(const std::vector<T>& was, const std::vector<std::size_t>& drop,
                        const std::vector<T>& add, std::vector<T>& out) {
        out.reserve(was.size() - drop.size() + add.size());
        std::size_t d = 0;
        for (std::size_t k = 0; k < was.size(); ++k) {
            if (d < drop.size() && drop[d] == k) {
                ++d;
                continue;
            }
            out.push_back(was[k]);
        }
        out.insert(out.end(), add.begin(), add.end());
    }

    // Clears [a, b) on a MIDI playlist, as overwrite() does on an audio one:
    // a region inside goes, one straddling an end is cut back to it, and
    // one spanning both is split, its right part taking `split_id` and
    // sharing the sequence. A cut moves the window, never the notes.
    bool midi_overwrite(MidiPlaylist& p, Ticks a, Ticks b, Id split_id, bool& used);

    bool move_midi(const MoveRegion& op);

    bool trim_midi(const TrimRegion& op);

    bool split_midi(const SplitRegion& op);

    // Copies the one MIDI playlist, lets `f` edit the copy and checks it,
    // sharing everything else with `before`. Every region of the copy still
    // shares its sequence with the original; only EditMidi and
    // PlaceMidiRegion bring a new one, and check it before they do. Unlike
    // edit_playlist() it copies inside a compound too: a MIDI playlist's
    // regions are a few words each, their notes are not copied.
    template <typename F>
    bool edit_midi_playlist(Id playlist, F&& f) {
        Located at;
        if (!locate_midi_playlist(*before_.edit, playlist, at)) {
            Located audio;
            if (locate_playlist(*before_.edit, playlist, audio)) {
                return fail("playlist " + str(playlist) + " is an audio playlist, not a MIDI one");
            }
            return fail("playlist " + str(playlist) + " does not exist");
        }
        const auto& track_ptr = before_.edit->tracks[at.track];
        MidiPlaylist copy = *track_ptr->midi_playlists[at.playlist];
        if (!f(copy)) return false;
        std::string where;
        const std::string why =
            detail::midi_playlist_problem(copy, *before_.sources, false, where);
        if (!why.empty()) return refuse_playlist(playlist, why, where);
        auto track = std::make_shared<TrackEdit>(*track_ptr);
        track->midi_playlists[at.playlist] = std::make_shared<MidiPlaylist>(std::move(copy));
        auto edit = std::make_shared<EditState>(*before_.edit);
        edit->tracks[at.track] = std::move(track);
        after_ = before_;
        after_.edit = std::move(edit);
        return true;
    }

    // A field of one audio or MIDI region that no other region, fade or
    // crossfade depends on: its name or colour.
    template <typename F>
    bool edit_region(Id playlist, Id region, F&& f) {
        Located midi;
        if (locate_midi_playlist(*before_.edit, playlist, midi)) {
            return edit_midi_playlist(playlist, [&](MidiPlaylist& p) {
                const std::size_t i = midi_region_index(p, region);
                if (i == kNone) return no_region(region, playlist);
                f(p.regions[i]);
                return true;
            });
        }
        return edit_playlist(playlist, [&](Playlist& p, std::uint16_t) {
            const std::size_t i = region_index(p, region);
            if (i == kNone) return no_region(region, playlist);
            f(p.regions[i]);
            mark(i, i);
            return true;
        });
    }

    bool no_insert(Id insert);

    std::size_t bus_index(Id bus) const;

    // Whether output channel `ch` is one of the pair starting at `first`.
    static bool in_pair(std::uint32_t first, std::uint32_t ch) { return ch == first || ch == first + 1; }

    std::string bus_name(Id bus) const;

    static bool has_send(const std::vector<Send>& sends, Id send);

    bool send_exists(Id send) const;

    bool no_send(Id send);

    static void set_send_mix(MixState& mix, const SendMix& m);

    static void set_bus_mix(MixState& mix, const BusMix& m);

    // `slot` on `t` holds nothing, or only the insert `moving` itself.
    bool free_slot(const TrackEdit& t, std::uint8_t slot, Id moving);

    static void set_insert_mix(MixState& mix, InsertMix m);

    // The track holding `insert` copied, its insert list edited by `f`.
    template <typename F>
    bool edit_insert(Id insert, F&& f) {
        InsertAt at;
        if (!locate_insert(*before_.edit, insert, at)) return no_insert(insert);
        return edit_track(before_.edit->tracks[at.track]->id,
                          [&](TrackEdit& t) { return f(t.inserts, at.index); });
    }

    // As edit_insert, for an edit that takes the insert's plugin lanes with
    // it (ADR-0027 §5, update of 2026-11-01): the plugin it keyed is gone.
    template <typename F>
    bool edit_insert_dropping_lanes(Id insert, F&& f) {
        InsertAt at;
        if (!locate_insert(*before_.edit, insert, at)) return no_insert(insert);
        return edit_track(before_.edit->tracks[at.track]->id, [&](TrackEdit& t) {
            if (!f(t.inserts, at.index)) return false;
            auto& lanes = t.automation;
            lanes.erase(std::remove_if(lanes.begin(), lanes.end(),
                                       [&](const std::shared_ptr<const AutomationLane>& l) {
                                           return l->param == AutomationParam::Plugin &&
                                                  l->insert == insert;
                                       }),
                        lanes.end());
            return true;
        });
    }

    // An insert's values, checked against its plugin's format. Only an
    // insert that is on a track: values left behind by a removal are kept
    // for undo, not edited.
    template <typename F>
    bool edit_insert_mix(Id insert, F&& f) {
        InsertAt at;
        if (!locate_insert(*before_.edit, insert, at)) return no_insert(insert);
        const Insert& in = before_.edit->tracks[at.track]->inserts[at.index];
        const InsertMix* existing = before_.mix.find_insert(insert);
        InsertMix m = existing != nullptr ? *existing : InsertMix{insert, false, {}, {}};
        if (!f(m)) return false;
        std::string where;
        const std::string why = detail::insert_mix_problem(m, in.plugin.format, where);
        if (!why.empty()) return fail("insert " + str(insert) + "." + where + ": " + why);
        after_ = before_;
        set_insert_mix(after_.mix, std::move(m));
        return true;
    }
    bool fail(std::string msg);

    bool no_region(Id region, Id playlist);

    // Ids come from the engine's counter, so every new one is at least
    // next_id. That makes "unique in the session" an O(1) check.
    bool fresh(Id id);

    static void set_mix(MixState& mix, const TrackMix& m);

    bool place(Id playlist, const Region& region, Id split_id,
               const std::vector<Crossfade>& crossfades);

    bool refuse_playlist(Id playlist, const std::string& why, const std::string& where);

    // The playlist operators. Each records what it changed in `touched_`,
    // which is all that edit_playlist() checks afterwards. In an audited
    // build's reference run they are the whole-playlist versions instead.
    bool overwrite(Playlist& p, Frames a, Frames b, Id split_id, bool& used);

    void insert_region(Playlist& p, Region r);

    void erase_region(Playlist& p, std::size_t i);

    void gone(Id region) { touched_.add_gone(region); }

    void mark(std::size_t first, std::size_t last) { touched_.mark(first, last); }

    // The two regions a crossfade on `p` joins.
    void mark_crossfade(const Playlist& p, const Crossfade& x);

    void drop_broken_crossfades(Playlist& p);

    void fit_fades(Playlist& p);

    std::string crossfade_problem(const Crossfade& x, const Playlist& p) const;

    // One that passed crossfade_problem(), so both its regions are there.
    void add_crossfade(Playlist& p, const Crossfade& x);

#if SONORA_MODEL_AUDIT
    // Audited builds (Debug, and the model's own tests) run every playlist
    // edit twice: through the whole-playlist operators and playlist_problem(),
    // as before BACKLOG Track 2 item 8, and through the range-local ones.
    // Both must reach the same verdict in the same words, and the same
    // playlist.
    struct Audit {
        bool ok = false;
        std::string error;
        Playlist playlist;
    };

    template <typename F>
    Audit run_reference(const Playlist& before, Id playlist, std::uint16_t channels, F& f) {
        Audit a;
        a.playlist = before;
        const std::string saved = error_;
        reference_ = true;
        a.ok = f(a.playlist, channels);
        reference_ = false;
        if (a.ok) {
            detail::sort_crossfades(a.playlist);
            std::string where;
            const std::string why =
                detail::playlist_problem(a.playlist, *before_.sources, channels, where);
            if (!why.empty()) a.ok = refuse_playlist(playlist, why, where);
        }
        a.error = error_;
        error_ = saved;
        return a;
    }

    [[noreturn]] static void audit_failed(const std::string& what);

    void audit_verdict(const Playlist& p, std::uint16_t channels, const std::string& why,
                       const std::string& where) const;

    void audit_against_reference(const Audit& a, bool ok, const Playlist& p) const;
#endif

    // Copies the one playlist, lets `f` edit the copy, checks the result
    // against every invariant where `f` could have broken one (local_edit.h),
    // and shares everything else with `before`.
    //
    // Inside a compound, a playlist an earlier member already copied is
    // edited where it is, and so are the track and edit state above it
    // (ADR-0009 update 2026-10-14, item 5): k members on one n-region
    // playlist then copy it once, not k times. Only when the whole chain
    // from the edit state down has one owner, which is then the fold's
    // working state: `before`, which the fold's caller holds, never does.
    //
    // The const_cast below is defined only because no Playlist, TrackEdit or
    // EditState anywhere in common/model is created const: every one is
    // make_shared<T>, never make_shared<const T> (the loader, the window
    // decoder and this file). Keep it that way.
    template <typename F>
    bool edit_playlist(Id playlist, F&& f) {
        Located at;
        if (!locate_playlist(*before_.edit, playlist, at)) {
            return fail("playlist " + str(playlist) + " does not exist");
        }
        const auto& track_ptr = before_.edit->tracks[at.track];
        const auto& playlist_ptr = track_ptr->playlists[at.playlist];
        const bool edit_mine = in_fold_ && before_.edit.use_count() == 1;
        const bool track_mine = edit_mine && track_ptr.use_count() == 1;
        const bool playlist_mine = track_mine && playlist_ptr.use_count() == 1;
        const std::uint16_t channels = track_ptr->channels;

        Playlist copy;
        if (!playlist_mine) copy = *playlist_ptr;
        Playlist& p = playlist_mine ? const_cast<Playlist&>(*playlist_ptr) : copy;
#if SONORA_MODEL_AUDIT
        const Audit reference = run_reference(*playlist_ptr, playlist, channels, f);
#endif
        touched_.clear();
        bool ok = f(p, channels);
        if (ok) {
            // The playlist was valid before the edit, so only what the edit
            // touched can be wrong now (local_edit.h). No crossfade sort either:
            // every operator keeps the list in region order.
            std::string where;
            const std::string why =
                detail::playlist_problem_near(p, *before_.sources, channels, touched_, where);
#if SONORA_MODEL_AUDIT
            audit_verdict(p, channels, why, where);
#endif
            if (!why.empty()) ok = refuse_playlist(playlist, why, where);
        }
#if SONORA_MODEL_AUDIT
        audit_against_reference(reference, ok, p);
#endif
        if (!ok) return false;
        if (playlist_mine) {
            after_ = before_;
            return true;
        }
        std::shared_ptr<const Playlist> made = std::make_shared<Playlist>(std::move(copy));
        if (track_mine) {
            const_cast<TrackEdit&>(*track_ptr).playlists[at.playlist] = std::move(made);
            after_ = before_;
            return true;
        }
        auto track = std::make_shared<TrackEdit>(*track_ptr);
        track->playlists[at.playlist] = std::move(made);
        if (edit_mine) {
            const_cast<EditState&>(*before_.edit).tracks[at.track] = std::move(track);
            after_ = before_;
            return true;
        }
        auto edit = std::make_shared<EditState>(*before_.edit);
        edit->tracks[at.track] = std::move(track);
        after_ = before_;
        after_.edit = std::move(edit);
        return true;
    }

    template <typename F>
    bool edit_track(Id track, F&& f) {
        const std::size_t t = locate_track(*before_.edit, track);
        if (t == kNone) return fail("track " + str(track) + " does not exist");
        auto copy = std::make_shared<TrackEdit>(*before_.edit->tracks[t]);
        next_id_ = before_.next_id;
        if (!f(*copy)) return false;
        auto edit = std::make_shared<EditState>(*before_.edit);
        edit->tracks[t] = std::move(copy);
        after_ = before_;
        after_.edit = std::move(edit);
        after_.next_id = next_id_;
        return true;
    }

    template <typename F>
    bool edit_markers(F&& f) {
        MarkerList m = *before_.edit->markers;
        next_id_ = before_.next_id;
        if (!f(m)) return false;
        detail::sort_markers(m);
        auto edit = std::make_shared<EditState>(*before_.edit);
        edit->markers = std::make_shared<const MarkerList>(std::move(m));
        after_ = before_;
        after_.edit = std::move(edit);
        after_.next_id = next_id_;
        return true;
    }

    const Snapshot& before_;
    Snapshot& after_;
    std::string& error_;
    const bool in_fold_;
    Id next_id_ = 0;
    detail::Touched touched_;
#if SONORA_MODEL_AUDIT
    bool reference_ = false;
#endif
};

}  // namespace sonora::model::apply_detail
