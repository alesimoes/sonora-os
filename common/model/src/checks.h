// SPDX-License-Identifier: GPL-3.0-or-later
//
// The session format's invariants, once. apply() runs them on what it
// built, check_invariants() on a whole snapshot, and the loader on what it
// read, so an edit can never produce a file the loader would refuse.
//
// Each returns an empty string when the value is fine, or the reason. `where`
// receives the offending element relative to the value checked
// ("regions[3]", "crossfades[0]"), for the caller to prefix with its path.
#pragma once

#include <string>
#include <vector>

#include "sonora/model/model.h"

namespace sonora::model::detail {

bool valid_text(const std::string& s);
std::string source_problem(const Source& s);
std::string region_problem(const Region& r, const SourcePool& pool,
                           std::uint16_t track_channels);
std::string crossfade_problem(const Crossfade& x, const Playlist& p,
                              const SourcePool& pool);
// The same, with the two regions already found (nullptr: not on the playlist).
std::string crossfade_problem(const Crossfade& x, const Region* left, const Region* right,
                              const SourcePool& pool);
std::string playlist_problem(const Playlist& p, const SourcePool& pool,
                             std::uint16_t track_channels, std::string& where);
// playlist_problem's parts, shared with the range-local check (local_edit.h)
// so both word a refusal the same way. order_problem: regions[i] against
// regions[i - 1]. heard_problem: a region's two ends, given the crossfades
// there (has_in: one ends `in_after` frames into it).
std::string order_problem(const Playlist& p, std::size_t i);
std::string second_crossfade_problem(const Crossfade& x);
extern const char* const kCrossfadesUnsorted;
std::string heard_problem(const Region& r, bool has_in, Frames in_after, bool has_out,
                          Frames out_before);
// By kind: an audio track's input has one entry per channel and its MIDI
// routing is the default; a MIDI or instrument track has no input; a MIDI
// track has no fader or pan either (the file has no key for them).
std::string mix_problem(const TrackMix& m, TrackKind kind, std::uint16_t track_channels,
                        std::string& where);
std::string midi_routing_problem(const MidiRouting& r, TrackKind kind, std::string& where);
std::string bus_problem(const MonitorBus& b, std::string& where);
std::string settings_problem(const SessionSettings& s, std::string& where);
std::string marker_problem(const Marker& m);
// Inserts (ADR-0012 §6). Values are checked against the format of the
// insert they belong to, which the InsertMix does not carry.
std::string plugin_problem(const PluginRef& p, std::string& where);
std::string inserts_problem(const std::vector<Insert>& inserts, std::string& where);
std::string insert_mix_problem(const InsertMix& m, PluginFormat format, std::string& where);
// Empty when `count` values fit one insert.
std::string insert_param_count_problem(std::size_t count);

// Sends and aux buses (ADR-0022). sends_problem: ids, slots in order and
// unique, one send per destination; not whether the destination exists.
std::string send_mix_problem(const SendMix& m, std::string& where);
std::string bus_mix_problem(const BusMix& m, std::string& where);
std::string sends_problem(const std::vector<Send>& sends, std::string& where);
const Bus* find_bus(const EditState& e, Id bus);
// The buses from `from` to `to` along sends, both included; empty when
// `to` cannot be reached.
std::vector<Id> bus_path(const EditState& e, Id from, Id to);
// Empty when the buses' sends form no loop; else `where` names a bus on one.
std::string bus_loop_problem(const EditState& e, std::string& where);

// Automation (ADR-0027). Points: each `at` from 0 to kMaxInteger, sorted, at
// most two at a frame, values in the param's range, at most
// kMaxAutomationPoints. A plugin value is finite, and from 0 to 1 when
// `format` is VST3. Lanes: sorted by lane_before, unique, none empty, none
// on a MIDI track, each plugin lane on an insert of its track with a key
// valid for that insert's format, at most kMaxInsertParams an insert.
std::string automation_points_problem(const std::vector<AutomationPoint>& points,
                                      AutomationParam param, std::string& where,
                                      PluginFormat format = PluginFormat::Lv2);
std::string automation_problem(const TrackEdit& t, std::string& where);
// Empty when (param, insert, key) names a lane `t` may hold; `format` is
// then the insert's format for a plugin lane.
std::string automation_target_problem(const TrackEdit& t, AutomationParam param, Id insert,
                                      const std::string& key, PluginFormat& format);
// Empty when `key` is a ParamValue key of `format`.
std::string param_key_problem(const std::string& key, PluginFormat format);

// A fade left running into the crossfade at its region's other end is
// shortened to meet it, as clamp_fades() shortens one that outgrew its
// region. Used by edits that move edges, clear a crossfade or add a take's
// crossfades, and by the loader for files older builds wrote. SetFade and
// SetCrossfade are refused instead (playlist_problem), because there the
// operator asked for exactly that length. Returns what it shortened.
struct FadeFit {
    std::size_t region = 0;   // index in Playlist::regions
    bool fade_in = false;     // else the fade-out
    Frames from = 0, to = 0;
};
std::vector<FadeFit> fit_fades_to_crossfades(Playlist& p);

// --- MIDI (midi.md §4.4) ---
std::string midi_source_problem(const MidiSource& s);
// Every value in range and every list in order. Linear in the sequence. A
// missing one (ADR-0015 O2) holds nothing and a well-formed reference.
// Lists from outside are checked as lists, before they are chunked: a
// MidiSequence places each element by its tick, so an order across chunks
// that was wrong no longer shows.
std::string sequence_problem(const MidiLists& s, std::string& where);
std::string sequence_problem(const MidiSequence& s, std::string& where);
std::string sequence_ref_problem(const SequenceFileRef& r);
// The region's own fields and its source, not its sequence: a sequence is
// checked once, when a region first holds it, and is immutable after.
std::string midi_region_problem(const MidiRegion& r, const SourcePool& pool);
std::string midi_order_problem(const MidiPlaylist& p, std::size_t i);
// With `check_data`, every region's sequence too: what the loader and
// check_invariants() ask; apply() checks only the sequences it makes.
std::string midi_playlist_problem(const MidiPlaylist& p, const SourcePool& pool, bool check_data,
                                  std::string& where);
// midi::sort_notes()'s order, which the sequence's notes are kept in.
bool note_before(const midi::Note& a, const midi::Note& b);
// Which playlists a track of its kind holds, and how many channels.
std::string track_shape_problem(const TrackEdit& t);
const char* track_kind_name(TrackKind kind);

const Region* find_region(const Playlist& p, Id id);
// Canonical order: by the left region's position.
void sort_crossfades(Playlist& p);
void sort_markers(MarkerList& m);

}  // namespace sonora::model::detail
