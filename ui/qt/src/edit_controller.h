// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Edit window's behaviour, without the window: edit modes, tools, zoom
// and scroll, selection, and turning a key or a mouse gesture into an
// operation on the TimelineDocument. No Qt, so every one of those can be
// tested by calling it, which is what test_edit does.
//
// The workflow is Pro Tools' (ADR-0004: reproduced deliberately, never its
// code): F1-F4 edit modes, the Smart Tool's zones, separate at the insertion
// point, trim to the insertion point, Grid snapping the region start.
//
// What is interface state and what is model state (ADR-0007 §5): the view
// (zoom, scroll, waveform scale), the tool, the mode and the selection are
// how the operator looks at the session, and live here. Regions, and mix,
// live in the document, and change only through submit().
//
// A gesture that a mode does not support yet is refused by name. Shuffle
// moves every later region when one changes length, and a Shuffle that
// moved only one would be a mode that lies about what it did.
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "automation_edit.h"
#include "edit_clipboard.h"
#include "time_format.h"
#include "timeline_document.h"

namespace sonora::ui {

enum class EditMode { Shuffle, Slip, Spot, Grid };
enum class Tool { Smart, Trim, Select, Grab, Pencil };

const char* edit_mode_name(EditMode m);
const char* tool_name(Tool t);

// Between an automation lane's edges and the highest and lowest value it
// draws, so a point at either end is not clipped.
constexpr double kAutolaneInset = 5.0;

struct ViewState {
    double fpp = 1200.0;    // frames per pixel: 40 px a second at 48 kHz
    double scroll = 0.0;    // timeline frame at the left edge
    double scroll_y = 0.0;  // pixels of track lanes above the top edge
    double width = 0.0;     // the track area, in pixels
    double height = 0.0;
    double track_height = 72.0;
    float wave_scale = 1.0f;  // vertical zoom of the waveforms
    // Automation lanes (ADR-0027) disclosed under a track make its row
    // taller; "lane" elsewhere in this file is a track's row. `tops[i]` is
    // track i's top in row pixels and `tops[n]` the end of the last; empty
    // when no track discloses lanes, and every track is then `track_height`
    // tall. `open[i]` says track i shows its lanes.
    static constexpr double kAutolaneBaseHeight = 56.0;  // at wave_scale 1
    double autolane_height = kAutolaneBaseHeight;
    std::vector<double> tops;
    std::vector<std::uint8_t> open;

    double x_of(model::Frames f) const { return (static_cast<double>(f) - scroll) / fpp; }
    model::Frames frame_at(double x) const;
    double y_of_track(int index) const;
    int track_at(double y) const;  // -1 above or below the lanes is the caller's check
    bool autolanes_open(int index) const {
        return index >= 0 && static_cast<std::size_t>(index) < open.size() && open[index] != 0;
    }
    // The top of automation lane `lane` (0 .. kLaneCount - 1) under track `index`.
    double y_of_autolane(int index, int lane) const {
        return y_of_track(index) + track_height + lane * autolane_height;
    }
    // The automation lane under pixel `y` of track `index`'s block: -1 over
    // the track's own row.
    int autolane_at(int index, double y) const;
    // A lane position (0 bottom to 1 top, automation_edit.h) as a y in
    // lane `lane` of track `index`, and back.
    double autolane_y(int index, int lane, float position) const {
        return y_of_autolane(index, lane) + kAutolaneInset +
               (1.0 - position) * (autolane_height - 2 * kAutolaneInset);
    }
    float autolane_position(int index, int lane, double y) const {
        const double p = 1.0 - (y - y_of_autolane(index, lane) - kAutolaneInset) /
                                   (autolane_height - 2 * kAutolaneInset);
        return static_cast<float>(std::clamp(p, 0.0, 1.0));
    }
    // The height of `tracks` rows, automation lanes included.
    double content_height(std::size_t tracks) const;
};

struct Selection {
    int track = -1;              // display index; -1 is none
    // How many tracks the insertion point or the range covers, from `track`
    // down, as a Selector drag across lanes or Select All leaves it (Pro
    // Tools' Edit selection on several tracks). A selected region is on
    // one track.
    int tracks = 1;
    model::Id region = kNoId;
    model::Frames insertion = 0;   // the edit cursor
    model::Frames start = 0, end = 0;  // a time range on those tracks; empty when equal
    // Automation: the lane of `track` (an index into the lanes a track
    // discloses) the range or the point belongs to, -1 for the track itself;
    // and that lane's point selected, -1 for none.
    int autolane = -1;
    int point = -1;

    bool has_range() const { return end > start; }
    bool covers(int index) const { return index >= track && index < track + tracks; }
};

// FadeIn / FadeOut: the Smart Tool's top corners, as in Pro Tools.
enum class HitZone { None, Lane, Body, Select, TrimStart, TrimEnd, FadeIn, FadeOut, Automation };

struct Hit {
    int track = -1;
    int autolane = -1;   // with HitZone::Automation
    model::Id region = kNoId;
    HitZone zone = HitZone::None;
};

// A gesture in progress: interface state, drawn outside the model until the
// release turns it into one operation (timeline-model.md).
struct Drag {
    // Point: a breakpoint added or moved; Stroke: the Pencil's line.
    enum class Kind { None, Move, TrimStart, TrimEnd, Range, FadeIn, FadeOut, Point, Stroke };
    Kind kind = Kind::None;
    // An automation gesture: its lane (-1 for the others); the point it
    // holds (-1 for a new one) and where it would be left; or the stroke
    // drawn so far.
    int autolane = -1;
    int point = -1;
    model::AutomationPoint at_point;
    std::vector<model::AutomationPoint> stroke;
    int track = -1;
    model::Id region = kNoId;
    model::Frames press_frame = 0;
    double press_x = 0, press_y = 0;
    model::Frames orig_start = 0, orig_end = 0;
    model::Frames start = 0, end = 0;  // the preview
    // A Range drag's other end: the lane under the pointer, so the range
    // covers every track from `track` to it.
    int track_end = -1;
    bool moved = false;
    // A fade drag's length as the drop would set it; 0 removes the fade.
    model::Frames fade = 0;
    // A trim's playlist as the drop would leave it, neighbours overwritten;
    // null when there is nothing to show beyond start and end.
    std::shared_ptr<const model::Playlist> preview;
};

class EditController {
public:
    explicit EditController(TimelineDocument& doc);

    TimelineDocument& document() { return doc_; }
    // After TimelineDocument::reset (a session was opened): the selection,
    // the drag and the scroll belonged to the old session and are dropped.
    // Zoom, mode, tool and grid are how the operator works and are kept.
    void document_replaced();
    const model::Snapshot& snapshot() const { return doc_.snapshot(); }

    // --- view ---
    const ViewState& view() const {
        refresh_rows();
        return view_;
    }
    void set_viewport(double width, double height);
    void set_track_height(double h);
    void zoom_in();
    void zoom_out();
    // `x` is the pixel kept still; the insertion point when it is in view,
    // else the centre.
    void zoom_by(double factor, double anchor_x);
    bool zoom_fit_session(std::string& why);
    bool zoom_fit_selection(std::string& why);
    bool zoom_toggle(std::string& why);
    void vertical_zoom(bool in);
    void scroll_by(double dx_px, double dy_px);
    void scroll_to_frame(double left_frame);
    // Page-scrolls so `playhead` stays on screen while the transport rolls.
    // Returns true when the view moved.
    bool follow(model::Frames playhead);
    double max_scroll_y() const;

    // --- modes, tools, grid ---
    EditMode mode() const { return mode_; }
    void set_mode(EditMode m) { mode_ = m; }
    void next_mode();
    Tool tool() const { return tool_; }
    void set_tool(Tool t) { tool_ = t; }
    void next_tool();
    GridValue grid() const { return grid_; }
    void set_grid(GridValue g) { grid_ = g; }
    bool grid_lines() const { return mode_ == EditMode::Grid; }

    // --- selection ---
    const Selection& selection() const { return sel_; }
    void select_track(int index);
    bool select_next_track(int delta, std::string& why);
    void set_insertion(model::Frames f);
    // As a click with the Grabber: the region, its track, and its bounds.
    bool select_region(model::Id region);
    // As a Selector drag from `a` to `b` leaves it, on `count` tracks from
    // `track` down (cut to the session's last track).
    void select_range(int track, model::Frames a, model::Frames b, int count = 1);
    // Pro Tools' Select All: every track, from the session start to the end
    // of its last region. Refused when there is no region to select.
    bool select_all(std::string& why);
    void clear_selection();
    // After tracks were removed elsewhere: the selection covers only tracks
    // that exist, so it never draws or edits a lane that is not there.
    void clamp_selection();

    // --- mouse, in track-area pixels ---
    Hit hit(double x, double y) const;
    bool press(double x, double y, std::string& why);
    void move(double x, double y);
    bool release(double x, double y, std::string& why);
    const Drag& drag() const { return drag_; }

    // --- edit commands; each one operation at most ---
    bool separate(std::string& why);
    bool trim_start_to_insertion(std::string& why);
    bool trim_end_to_insertion(std::string& why);
    // A region selected, or a time range on the selected track: removes
    // what the range covers, leaving the gap (Slip, Spot and Grid). A range
    // that is exactly one fade or crossfade removes that instead.
    bool clear(std::string& why);
    // Thin the selected lane's range (`automation.thin`); refused with a
    // reason when no lane range is selected or nothing would go.
    bool thin_automation(std::string& why);
    bool nudge(int direction, std::string& why);

    // --- the clipboard (edit_clipboard.h) ---
    // On a selected region, or a time range on the selected tracks, every
    // track of it in the same compound. Each
    // edit is one compound, so one undo step labelled with its name; one
    // that needs more than model::kMaxCompoundMembers members is refused
    // before anything is sent. Shuffle would ripple every later region,
    // which waits for ShiftRegions, so Cut, Paste, Duplicate and Repeat
    // refuse it.
    //
    // Copy changes no document and is no undo step, as in Pro Tools.
    bool copy(std::string& why);
    bool cut(std::string& why);
    // At the selection's start, the clipboard's first lane on the selected
    // track and each next lane on the track below, overwriting what the
    // clipboard's whole length covers there; the pasted range is then
    // selected. Lanes with no track below to land on are left out, as Pro
    // Tools leaves them: the paste goes ahead, and `why` then names what was
    // not pasted, for the status line (empty when everything was).
    bool paste(std::string& why);
    // A copy of the selection placed straight after it on the same tracks,
    // which then becomes the selection. The clipboard is left alone.
    bool duplicate(std::string& why);
    // Pro Tools' Repeat: `count` copies of the selection end to end straight
    // after it, as one compound ("Repeat"), so one undo step. What they
    // cover is overwritten, as Duplicate's copy overwrites. The copies then
    // become the selection, as Duplicate's does. A count below 1 is refused;
    // so is one needing more than model::kMaxCompoundMembers members, by
    // the count, before anything is built.
    bool repeat(int count, std::string& why);
    // True when Repeat has something to repeat; otherwise false and why
    // not, as repeat() would say it. The count dialog opens only when this is
    // empty, so the operator is not asked for a number and then refused.
    // `track` and `length`, when given, are what one copy would be: the
    // names of the tracks it lands on and the range's length, for the dialog.
    bool can_repeat(std::string& why, std::string* track = nullptr,
                    model::Frames* length = nullptr) const;
    // The count the last Repeat used, which the dialog offers next.
    int repeat_count() const { return repeat_count_; }
    const Clipboard& clipboard() const { return clipboard_; }
    // Changes whenever the clipboard is replaced.
    uint64_t clipboard_serial() const { return clipboard_serial_; }
    // Fades (Pro Tools' Fade to Start, Fade to End, and the crossfade
    // without its dialog). Fades change no region's position, so every edit
    // mode allows them. The shape is the last one used, as Pro Tools'
    // no-dialog fades are; the window says which.
    bool fade_to_start(std::string& why);
    bool fade_to_end(std::string& why);
    // On the selected track's time range: across one cut between two
    // butted regions, a crossfade (centred; pre or post when the range ends
    // or starts at the cut); over one region's start, a fade-in; over its
    // end, a fade-out. Anything else is refused by name rather than guessed.
    // The selection becomes what was made, so Clear takes it away again.
    // A crossfade longer than the handles at the cut is shortened to them
    // (ADR-0021): true, with `why` saying what was made instead.
    bool fade_selection(std::string& why);
    model::FadeShape fade_shape() const { return fade_shape_; }
    model::FadeShape crossfade_shape() const { return crossfade_shape_; }
    void set_fade_shape(model::FadeShape s) { fade_shape_ = s; }
    void set_crossfade_shape(model::FadeShape s) { crossfade_shape_ = s; }
    bool undo(std::string& why);
    bool redo(std::string& why);
    // The Undo History window's jump: undo or redo, one step at a time, until
    // History::undo_depth() is `depth`, as Pro Tools does from its list.
    // Each step is its own Undo or Redo, as pressing the key that many times
    // would be. A refusal here (offline, behind the engine) comes at the
    // first step, before anything is sent, so the jump does nothing. On the
    // engine's document every step is sent at once: a step the engine
    // refuses later is reported on its own (ReplicaDocument::on_refused),
    // and the steps after it have already gone. A depth past the newest step
    // is refused.
    bool undo_to(std::size_t depth, std::string& why);

    // --- automation lanes (ADR-0027) ---
    //
    // A track discloses a lane for each parameter the model automates
    // (kLaneCount, top to bottom) under its row; whether it does is how the
    // operator looks at the session, so it is interface state, dropped when
    // a session opens. Every edit below is one EditAutomation, so one undo
    // step; a gesture with the mouse is previewed in the Drag and sent at the
    // release. The Smart Tool and the Grabber add a breakpoint where the lane
    // is pressed or move the one there; the Pencil draws freely (thinned at
    // the release); the Selector selects a range of the lane, which Cut,
    // Copy, Paste and Clear then act on instead of the regions. Clear on a
    // selected breakpoint deletes it.
    bool autolanes_shown(int track) const;
    // Refused on a MIDI track, which has no audio to automate.
    bool toggle_autolanes(int track, std::string& why);
    // The lane under the selection: its track's edit state, or null.
    const model::AutomationLane* selected_lane() const;
    const AutomationClip& automation_clipboard() const { return auto_clip_; }

    // --- markers: Pro Tools' Memory Locations (edit_markers.cpp) ---
    //
    // A marker is session state: adding or clearing one is an operation and
    // an undo step ("New Marker", "Clear Marker"). Recalling one is how the
    // operator looks at the session, so it changes the selection only.
    //
    // Where a new marker goes: with the transport rolling, the playhead
    // (`rolling` true), as Pro Tools drops one on the fly; else a selected
    // range becomes a selection marker over it (Pro Tools' Selection memory
    // location), and an insertion point a marker there.
    struct MarkerPlace {
        model::Frames position = 0;
        model::Frames length = 0;
    };
    MarkerPlace marker_place(bool rolling, model::Frames playhead) const;
    // The name a new marker is offered: "Marker N", the first N no marker
    // of the session is called already.
    std::string next_marker_name() const;
    // `name` trimmed; an empty one is next_marker_name().
    bool add_marker(const std::string& name, MarkerPlace at, std::string& why);
    bool remove_marker(model::Id marker, std::string& why);
    // A marker: the insertion point moves to it; a selection marker: its
    // range is selected, on the selected tracks (the first track when none
    // is). The view scrolls to it when it is off screen. Nothing is snapped
    // to the grid: the marker is where it was put. The playhead follows,
    // to the marker's start, through the locator.
    bool recall_marker(model::Id marker, std::string& why);
    // Counter entry: the insertion point moves to `frame` exactly (typed,
    // so never snapped to the grid), the view pages to it, and the playhead
    // follows through the locator, as a marker recall does.
    void locate_to(model::Frames frame);
    // Where recall sends the playhead: transport.locate, wired by
    // link_marker_locate(). None, recall moves the selection only.
    using Locator = std::function<void(model::Frames frame)>;
    void set_locator(Locator l) { locate_ = std::move(l); }
    // The marker drawn under `x` on the marker ruler, in its pixels: a
    // marker's flag, else a selection marker's bar; kNoId over neither.
    model::Id marker_at(double x) const;
    const model::MarkerList& markers() const { return *snapshot().edit->markers; }

    // --- tracks: Pro Tools' Track menu (ADR-0023), each one undo step ---
    // `count` mono audio tracks named "Audio <n>" with the lowest free
    // numbers, each on the lowest capture input no audio track uses, below
    // the selected track (at the end with none). The first is selected.
    bool new_tracks(int count, std::string& why);
    // The track's active playlist, plug-ins and mix copied onto a new track
    // below it, named "<name>.dup<n>" (Pro Tools' Duplicate Tracks with its
    // default choices). The copy is selected.
    bool duplicate_track(int track, std::string& why);
    bool delete_track(int track, std::string& why);
    // One row up (delta -1) or down (+1); the selection goes with it.
    bool move_track(int track, int delta, std::string& why);

    // --- playlists: the track's playlist selector (ADR-0026) ---
    // Each one undo step. New and Duplicate name the playlist
    // "<track>.<nn>" (next_playlist_name()) and make it the active one.
    // The names of a track's playlists in their order, and its active one.
    std::vector<std::string> playlist_names(int track) const;
    std::string active_playlist_name(int track) const;
    bool new_playlist(int track, std::string& why);
    bool duplicate_playlist(int track, std::string& why);
    bool choose_playlist(int track, const std::string& name, std::string& why);
    // The active playlist renamed; refused empty, or as another playlist of
    // the session is named.
    bool rename_playlist(int track, const std::string& name, std::string& why);
    // Every playlist but the active one off the track; their audio stays in
    // the pool, and undo brings them back.
    bool delete_unused_playlists(int track, std::string& why);

    // --- mix: not undoable, but still operations ---
    bool set_gain(int track, float db, std::string& why);
    bool set_pan(int track, float pan, std::string& why);
    bool toggle_mute(int track, std::string& why);
    bool toggle_solo(int track, std::string& why);
    // Solo safe (TrackMix::solo_safe): another track's solo never silences
    // this one. Its own solo and mute are left as they are.
    bool toggle_solo_safe(int track, std::string& why);
    // A track's automation mode (ADR-0027). Trim is refused: the engine has
    // no write slice for it yet.
    bool set_automation_mode(int track, model::AutomationMode mode, std::string& why);
    // Pro Tools' TrackInput: Input mode (the live input whatever the
    // transport does) or back to Auto. Off, which only a file can set, is
    // not Input, so it goes to Input. Refused on a MIDI or instrument
    // track, whose engine track carries no audio of its own (ADR-0014 M-1).
    bool toggle_input_monitor(int track, std::string& why);
    // Off, Read, Touch, Latch, Write and round to Off. Trim is skipped (the
    // engine refuses it); a track already on Trim steps to Off.
    bool step_automation_mode(int track, std::string& why);
    bool set_bus_gain(float db, std::string& why);

    // The end of the last region, or one minute when the session is empty.
    model::Frames session_end() const;
    // The playlist and region under `frame` on display track `track`.
    const model::Region* region_at(int track, model::Frames frame) const;

private:
    bool submit(const model::Operation& op, std::string& why);
    bool refuse_in_mode(const char* what, std::string& why) const;
    model::Frames snapped(model::Frames f) const;
    void clamp_scroll();
    // Pages the view so `frame` is a tenth in from the left edge, when it
    // is off screen (as follow() pages).
    void reveal(model::Frames frame);
    const model::TrackEdit* track(int index) const;
    // The region a command acts on: the selected one, else the one under
    // the insertion point on the selected track.
    const model::Region* target_region(int& track_index) const;
    // A single fade on `region`'s `end`, with the refusals every fade path
    // shares: a crossfaded end, a length past the other end's fade.
    bool set_region_fade(model::Id region, model::FadeEnd end, model::Frames length,
                         std::string& why);
    // Clear on a range that is exactly one fade or crossfade removes it.
    bool clear_fade_at_selection(bool& done, std::string& why);
    // The tracks and range a clipboard edit acts on.
    struct Span {
        int first = -1, count = 0;
        model::Frames a = 0, b = 0;
    };
    // The selection's tracks that exist: none without a selected track.
    int span() const;
    // A command that acts on one track, refused by name on several rather
    // than done on the top one only.
    bool refuse_several(const char* gesture, std::string& why) const;
    // A selected region's bounds on its track, else the selected tracks'
    // time range. `nothing_was` finishes the refusal ("... Nothing was cut.").
    bool edit_range(Span& span, const char* nothing_was, std::string& why) const;
    // One lane per track of `span`; false, and why, when no lane holds a
    // region.
    bool copy_lanes(const Span& span, Clipboard& out, const char* nothing_was,
                    std::string& why) const;
    // `span` becomes the selection: a range on its tracks, no region.
    void select_span(int first, int count, model::Frames a, model::Frames b);
    bool refuse_shuffle(const char* gesture, const char* nothing_was, std::string& why) const;
    // One gesture's members as one Compound labelled `gesture`, refused
    // unsent past the member limit.
    bool submit_compound(std::vector<model::Member> members, const char* gesture,
                         const char* nothing_was, std::string& why);

    TimelineDocument& doc_;
    void refresh_rows() const;
    bool autolane_selected() const { return sel_.autolane >= 0; }
    // Cut, Copy, Paste and Clear on the selected lane's range.
    bool automation_copy(bool cut, std::string& why);
    bool automation_paste(std::string& why);
    bool automation_clear(std::string& why);
    bool press_autolane(const Hit& h, double x, double y, std::string& why);
    bool release_autolane(const Drag& d, std::string& why);
    void move_autolane(double x, double y);
    // The lane value under pixel `y` of lane `lane` of track `index`.
    float autolane_value_at(int index, int lane, double y) const;

    mutable ViewState view_;
    ViewState saved_view_;
    bool has_saved_view_ = false;
    EditMode mode_ = EditMode::Slip;
    Tool tool_ = Tool::Smart;
    GridValue grid_ = GridValue::Beat;
    Selection sel_;
    Drag drag_;
    model::FadeShape fade_shape_ = model::FadeShape::Linear;
    model::FadeShape crossfade_shape_ = model::FadeShape::EqualPower;
    Clipboard clipboard_;
    AutomationClip auto_clip_;
    std::set<model::Id> autolanes_open_;
    int repeat_count_ = 1;
    Locator locate_;
    uint64_t clipboard_serial_ = 0;
};

// --- fades, as the engine plays them ---
//
// The gain at `x` (0..1 through a fade-in) for each shape: the engine's own
// sonora::fade_in_gain() from common/base's sonora/fade_curve.h, the one
// definition engine/core plays, so the curve the timeline draws cannot drift
// from the gain the operator hears. engine/core's test_playback_mix pins the
// law's values.
float fade_gain(model::FadeShape shape, float x);
const char* fade_shape_name(model::FadeShape shape);
model::FadeShape next_fade_shape(model::FadeShape shape);

// Where a region's own fades are heard. A crossfade at an end replaces that
// end's fade (engine/control's plan builder), so it is 0 there and the
// crossfade is drawn instead: drawing both would show a fade nobody hears.
struct RegionFades {
    model::Frames in = 0, out = 0;
    model::FadeShape in_shape = model::FadeShape::Linear;
    model::FadeShape out_shape = model::FadeShape::Linear;
    const model::Crossfade* in_crossfade = nullptr;   // at the region's start
    const model::Crossfade* out_crossfade = nullptr;  // at its end
};
RegionFades region_fades(const model::Playlist& p, const model::Region& r);

// --- resolvers: command + snapshot -> one fully resolved operation ---
//
// These are what engine/control will run when it resolves a command
// (timeline-model.md, "Operations"). They clamp to what the model allows,
// so a gesture that went too far becomes the nearest valid edit instead of
// an error: a trim stops where the source's audio runs out, and a trim or a
// move into a neighbour overwrites it, as Slip and Grid do. Ids come from the caller (`spare_id`, used only when the move
// lands inside another region and must split it); a resolver never invents
// one.
bool resolve_move(const model::Snapshot& s, model::Id region, model::Frames to,
                  model::Id spare_id, model::MoveRegion& out);
bool resolve_trim(const model::Snapshot& s, model::Id region, model::Edge edge,
                  model::Frames to, model::TrimRegion& out);

}  // namespace sonora::ui
