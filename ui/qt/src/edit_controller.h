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
#include <utility>
#include <vector>

#include "automation_edit.h"
#include "sonora/model/arrangement.h"
#include "edit_clipboard.h"
#include "peak_source.h"
#include "sample_view.h"
#include "strip_silence.h"
#include "time_format.h"
#include "timeline_document.h"
#include "track_row_geometry.h"

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
    // Each track's own row, apart from its lanes (ADR-0106): empty while
    // every row is `track_height`, 0 for a hidden track.
    std::vector<double> heights;
    double row_h(int index) const {
        return index >= 0 && static_cast<std::size_t>(index) < heights.size()
                   ? heights[static_cast<std::size_t>(index)]
                   : track_height;
    }
    bool row_hidden(int index) const { return row_h(index) <= 0.0; }
    // Take lanes (ADR-0031 §1, ADR-0049): a track's other playlists shown
    // under its row, below its automation lanes, each `comp_lane_height`
    // tall. `lanes[i]` is how many track i shows; empty when none does.
    static constexpr double kCompLaneBaseHeight = 46.0;  // at wave_scale 1
    double comp_lane_height = kCompLaneBaseHeight;
    std::vector<std::uint8_t> lanes;
    int lanes_shown(int index) const {
        return index >= 0 && static_cast<std::size_t>(index) < lanes.size() ? lanes[index] : 0;
    }
    // The top of take lane `lane` (0 first) under track `index`.
    double y_of_complane(int index, int lane) const {
        return y_of_track(index) + row_h(index) +
               (autolanes_open(index) ? kLaneCount * autolane_height : 0.0) +
               lane * comp_lane_height;
    }
    // The take lane under pixel `y`, -1 over the track's own row or its
    // automation lanes.
    int complane_at(int index, double y) const {
        const int n = lanes_shown(index);
        if (n == 0) return -1;
        const double rel = y - y_of_complane(index, 0);
        if (rel < 0) return -1;
        return std::min(n - 1, static_cast<int>(rel / comp_lane_height));
    }

    double x_of(model::Frames f) const { return (static_cast<double>(f) - scroll) / fpp; }
    model::Frames frame_at(double x) const;
    double y_of_track(int index) const;
    int track_at(double y) const;  // -1 above or below the lanes is the caller's check
    bool autolanes_open(int index) const {
        return index >= 0 && static_cast<std::size_t>(index) < open.size() && open[index] != 0;
    }
    // The top of automation lane `lane` (0 .. kLaneCount - 1) under track `index`.
    double y_of_autolane(int index, int lane) const {
        return y_of_track(index) + row_h(index) + lane * autolane_height;
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
enum class HitZone {
    None, Lane, Body, Select, TrimStart, TrimEnd, FadeIn, FadeOut, Automation,
    TakeLane   // a take lane under the track: Hit::lane, and its region if one is there
};

struct Hit {
    int track = -1;
    int autolane = -1;   // with HitZone::Automation
    int lane = -1;       // with HitZone::TakeLane: an index into take_lane_ids()
    model::Id region = kNoId;
    HitZone zone = HitZone::None;
};

// A gesture in progress: interface state, drawn outside the model until the
// release turns it into one operation (timeline-model.md).
struct Drag {
    // Point: a breakpoint added or moved; Stroke: the Pencil's line.
    // Swipe: a range dragged across a take lane (`lane` its playlist).
    // Pencil: the Pencil on a clip's waveform at sample zoom (ADR-0096).
    enum class Kind { None, Move, TrimStart, TrimEnd, Range, FadeIn, FadeOut, Point, Stroke, Swipe,
                      Pencil };
    Kind kind = Kind::None;
    model::Id lane = 0;
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
    // A Pencil stroke on `region`: the pointer positions so far, the preview,
    // and the region channel it draws (from 0; the lane it was pressed on).
    std::vector<PencilPosition> pencil;
    uint16_t channel = 0;
};

// One channel's lane of a clip in a track row at [row_top, row_top +
// row_height): the waveform area under the name band, split evenly between
// `count` channels, as the sample view draws them. The inset is the
// timeline's lane inset (timeline_scene.cpp) and the band theme's
// kRegionNameBand, so a level read from a y is the level drawn there.
struct SampleLane {
    double top = 0.0, height = 0.0;
    double mid() const { return top + height / 2; }
};
SampleLane sample_lane(double row_top, double row_height, int count, int lane);

class MarkerViewStore;

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
    // Per-track heights and hidden rows (ADR-0106); not owned, null for plain rows.
    void set_row_policy(const RowPolicy* p) { rows_ = p; }
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
    // Pro Tools' Automation Follows Edit (Options menu), on by default as
    // there: a region moved or cleared takes the automation under it along
    // (ADR-0027 §7). How the operator works, so kept when a session opens.
    bool automation_follows_edit() const { return automation_follows_edit_; }
    void set_automation_follows_edit(bool on) { automation_follows_edit_ = on; }
    // What the last edit went ahead without, named, so the operator learns
    // the automation did not go where the edit implied: the lanes a region
    // move or clear left behind, too full to follow it (follow_move_ops),
    // and the tracks a lane range covered but whose lanes are hidden, or a
    // paste had no track for. Shown held, not on the status line. Empty
    // once taken, and when nothing was left out.
    std::string take_follow_notice() { return std::exchange(follow_notice_, std::string()); }

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
    // Pro Tools' Heal Separation: joins regions that butt at a cut the
    // insertion point sits on, or the range or selected region covers, when
    // they still play one stretch of one source. Refused, naming the pair,
    // when the join would change what plays (moved, faded, gain, mute).
    bool heal_separation(std::string& why);
    // Strip Silence on the selected region, or the regions a selected range
    // covers whole: each stretch that stays under the threshold for the
    // minimum time is separated out and removed, leaving gaps (Slip, Spot,
    // Grid). One undo step. Refused, naming the threshold, when nothing is
    // quiet enough or everything is.
    bool strip_silence(const PeakSource& peaks, const StripParams& params, std::string& why);
    bool trim_start_to_insertion(std::string& why);
    bool trim_end_to_insertion(std::string& why);
    // A region selected, or a time range on the selected track: removes
    // what the range covers, leaving the gap (Slip, Spot and Grid). A range
    // that is exactly one fade or crossfade removes that instead.
    bool clear(std::string& why);
    // Thin the selected lane's range (`automation.thin`); refused with a
    // reason when no lane range is selected or nothing would go.
    bool thin_automation(std::string& why);
    // Write to Selection (`automation.write_selection`): the selected lane
    // range held flat at the track's fader or pan as it stands now (Volume
    // Trim: 0 dB). Refused with a reason when no lane range is selected.
    bool write_automation_to_selection(std::string& why);
    // Snapshot (`automation.snapshot`): one breakpoint at the insertion point
    // on the event list's lane of each selected track, holding the fader or
    // pan as it stands now (Volume Trim: 0 dB). One undo step.
    bool snapshot_automation(std::string& why);

    // --- the Event List (`window.event_list`) ---
    // The lane it lists, 0..kLaneCount-1, on the selected track: the lane
    // last touched on the timeline, or the one its picker chose.
    int event_lane() const { return event_lane_; }
    void set_event_lane(int lane) {
        if (lane >= 0 && lane < kLaneCount) event_lane_ = lane;
    }
    // The listed lane's points, null when the track has none there.
    const model::AutomationLane* event_points() const;
    // Point `row` moved to `at` and given `value`, as one undo step; it keeps
    // its place between its neighbours (moved_point). A step's two points
    // keep their frame.
    bool event_edit(std::size_t row, model::Frames at, float value, std::string& why);
    // Point `row` removed, one undo step.
    bool event_delete(std::size_t row, std::string& why);
    bool nudge(int direction, std::string& why);
    // Pro Tools' Insert Time and Delete Time on the selected range of the
    // selected tracks (a selected region counts as its bounds): the range's
    // length is opened at its start, or taken out, in every edit mode.
    // Regions of those tracks that start at or after it move by the length
    // (a straddling one is split, or cut, first), and so do the automation
    // points after it on those tracks and every marker, all in one compound,
    // one undo step. Delete Time takes markers inside the range to its start.
    bool insert_time(std::string& why);
    bool delete_time(std::string& why);
    // Spot mode: the selected region placed so that it starts exactly at
    // `to`, typed, never snapped (Pro Tools' Spot dialog, start time). What
    // it lands on is overwritten, as a drag in Slip does. Refused in Shuffle.
    bool spot_region(model::Frames to, std::string& why);
    // A click on a region in Spot mode leaves the region here for the
    // window to ask where it starts; kNoId once taken, and when no click
    // asked.
    model::Id take_spot_request() { return std::exchange(spot_request_, kNoId); }

    // --- the clipboard (edit_clipboard.h) ---
    // On a selected region, or a time range on the selected tracks, every
    // track of it in the same compound. Each
    // edit is one compound, so one undo step labelled with its name; one
    // that needs more than model::kMaxCompoundMembers members is refused
    // before anything is sent. In Shuffle, Clear and Cut close the gap, and
    // Paste, Duplicate and Repeat open one, by moving every later region
    // (ShiftRegions) inside the same compound.
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
    // Copy, Paste, Clear and Thin then act on instead of the regions.
    // Dragged across tracks, the range covers that lane on each (shown
    // tracks only), and their edits go as one Compound, so one undo step.
    // Clear on a selected breakpoint deletes it.
    bool autolanes_shown(int track) const;
    // Refused on a MIDI track, which has no audio to automate.
    bool toggle_autolanes(int track, std::string& why);
    // The lane under the selection: its track's edit state, or null.
    const model::AutomationLane* selected_lane() const;
    // One clip a track the copied range covered, top down; a track with
    // nothing to copy (no automation, its lanes hidden) holds an empty one,
    // so a paste still lands every other clip on its own track.
    const std::vector<AutomationClip>& automation_clipboard() const { return auto_clips_; }

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
    // The Edit window's view stored with a marker (ADR-0137): interface
    // state kept by `store`, not owned. Recall applies what the marker's
    // Zoom and Track layout toggles tick, after the locate. `hide` receives
    // the track ids to hide (ids that no longer exist already dropped); it
    // is what applies the track layout, since the rows belong to the track
    // list. With neither wired, a recall moves the position only.
    using TrackHider = std::function<void(const std::vector<std::uint64_t>& hidden)>;
    void set_marker_views(MarkerViewStore* store, TrackHider hide) {
        marker_views_ = store;
        hide_tracks_ = std::move(hide);
    }
    bool marker_has_view(model::Id marker) const;
    // Captures zoom, left edge, track height and hidden tracks now.
    bool store_marker_view(model::Id marker, std::string& why);
    bool clear_marker_view(model::Id marker, std::string& why);
    // `zoom` true sets the Zoom toggle, false the Track layout one.
    bool set_marker_view_part(model::Id marker, bool zoom, bool on, std::string& why);
    // The marker drawn under `x` on the marker ruler, in its pixels: a
    // marker's flag, else a selection marker's bar; kNoId over neither.
    model::Id marker_at(double x) const;
    const model::MarkerList& markers() const { return *snapshot().edit->markers; }

    // --- arrangement sections (edit_arrangement.cpp, ADR-0116) ---
    // The session's range markers: the sections Move to / Copy to act on.
    std::vector<model::Marker> arrangement_sections() const;
    // Moves or copies `section` to the timeline frame `destination` as one
    // Compound, one undo step. `message` is set either way: on success what
    // the edit left where it was, else the refusal (nothing was changed).
    bool arrangement_apply(model::Id section, model::Frames destination, model::SectionMode mode,
                           std::string& message);

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

    // --- names and colours (ADR-0023, update of 2026-11-02) ---
    // Each one undo step. A track name is refused empty or as another
    // track is named, as Pro Tools refuses it; surrounding spaces go.
    bool rename_track(int track, const std::string& name, std::string& why);
    // `colour` a palette index (model::kColours of them), on `count`
    // tracks from `track` down, as one step: Pro Tools colours every
    // selected track at once.
    bool set_track_colour(int track, int count, int colour, std::string& why);
    // The region Rename and Colour act on: the one selected, or why there
    // is none. Never the one under the insertion point, which the operator
    // did not pick.
    const model::Region* selected_region(std::string& why) const;
    bool rename_region(const std::string& name, std::string& why);
    // A palette index, or model::kTrackColour for the track's.
    bool set_region_colour(int colour, std::string& why);
    // --- clip gain (edit_clip_gain.cpp) ---
    // On the selected region, each one undo step. set_clip_gain is the
    // region's own level in dB (-144 to +24); a nudge adds to it and is
    // refused at the limit. A gain point sits at a timeline frame inside
    // the region, `db` its level on top of the clip's; the first one also
    // holds both ends at 0 dB. The line is stored in source frames.
    bool set_clip_gain(float db, std::string& why);
    bool nudge_clip_gain(float delta_db, std::string& why);
    bool set_gain_point(model::Frames at, float db, std::string& why);
    bool remove_gain_point(model::Frames at, std::string& why);
    bool clear_gain_line(std::string& why);
    // --- tempo (edit_tempo.cpp, ADR-0050) ---
    // Tempo edits are session settings, outside the edit undo: each one is
    // a SetSettings with the tempo map replaced, and tempo_undo / tempo_redo
    // step through them on a stack of their own, emptied when the session is
    // replaced or the map is changed by anyone else.
    bool set_tempo(model::Frames at, float bpm, std::string& why);
    // bpm_from <= 0 starts the ramp at the tempo in force at `from`.
    bool ramp_tempo(model::Frames from, model::Frames to, float bpm_from, float bpm_to,
                    std::string& why);
    bool scale_tempo(model::Frames from, model::Frames to, double factor, std::string& why);
    // The tempo change nearest `at`, the first excepted.
    bool remove_tempo(model::Frames at, std::string& why);
    // On the bar line nearest `near`.
    bool set_meter(model::Frames near, std::uint32_t beats, std::uint32_t note, std::string& why);
    bool tempo_undo(std::string& why);
    bool tempo_redo(std::string& why);
    // What the Tempo dialog and the tempo.edit command take, at the start of
    // the range or else the insertion point:
    //   "96"      the tempo becomes 96 bpm there
    //   "120>90"  ramp from 120 to 90 over the range (">90": from the
    //             tempo in force)
    //   "x2"      the range at double the tempo ("x0.5": half)
    //   "3/4"     a 3/4 bar starts on the nearest bar line
    //   "remove"  the tempo change nearest the insertion point goes
    bool edit_tempo(const std::string& text, std::string& why);
    bool can_tempo_undo() const { return !tempo_undo_.empty(); }
    bool can_tempo_redo() const { return !tempo_redo_.empty(); }
    // The line's level (dB, 0 with none) at a timeline frame of `r`.
    static float clip_line_db_at(const model::Region& r, model::Frames timeline_frame);
    // --- clip groups, loops and sample edits (edit_clip_groups.cpp; ADR-0052) ---
    // Group joins the clips the range selection touches on the selected track
    // (two or more, none already grouped); Ungroup frees the selected clip's
    // group. Loop lays `count` repeats after the selected clip, or after its
    // group, taking out the repeats an earlier Loop laid; Unloop takes them
    // out. A group moves as one (Move, Nudge): the same distance for every
    // member, stopping at the session start. Each is one undo step.
    bool group_clips(std::string& why);
    bool ungroup_clip(std::string& why);
    bool loop_clip(int count, std::string& why);
    bool unloop_clip(std::string& why);
    int loop_count() const { return loop_count_; }
    // The engine's sample.edit argument for `verb` on the range selection,
    // which must lie inside one clip; otherwise why not.
    bool sample_edit_argument(const std::string& verb, const std::string& param,
                              std::string& argument, std::string& why) const;
    // --- the sample view and the Pencil on the waveform (ADR-0096) ---
    // At kSampleViewMaxFpp or closer the sample view replaces the overview.
    bool sample_zoom() const { return view_.fpp <= kSampleViewMaxFpp; }
    // True where the sample view shows: at sample zoom, over a clip on a
    // track's own row.
    bool sample_view_at(double x, double y) const;
    // What the Pencil checks before it draws (ready(), so it never draws over
    // a window still loading); none, and it does not check.
    void set_sample_source(const SampleSource* s) { samples_ = s; }
    // What the timeline draws the sample view from (sample_scene.h).
    const SampleSource* sample_source() const { return samples_; }
    // Where a finished stroke goes: the argument of one `sample.edit`. None,
    // and a stroke is refused: there is no engine to draw.
    using SampleEditSender = std::function<void(const std::string& argument)>;
    void set_sample_edit_sender(SampleEditSender s) { sample_sender_ = std::move(s); }
    // Whether a take is recording. The engine refuses sample.edit then
    // (sample_edit.h); the Pencil refuses before sending, so nothing is drawn
    // on screen that the engine will not write.
    void set_recording(bool on) { recording_ = on; }
    bool recording() const { return recording_; }
    // The `sample.edit draw REGION FROM TO CHANNEL POINTS` argument a Pencil
    // drag `d` would send (pencil_points), cut at its clip's edges; otherwise
    // why not.
    bool pencil_argument(const Drag& d, std::string& argument, std::string& why) const;
    // The palette index a region is drawn in: its own, else its track's.
    static int region_colour(const model::TrackEdit& t, const model::Region& r) {
        return r.colour == model::kTrackColour ? t.colour : r.colour;
    }

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

    // --- take lanes and comping (edit_comping.cpp, ADR-0031, ADR-0049) ---
    //
    // A track's lanes are its playlists other than the active one (the
    // comp). Showing them is how the operator looks at the session, so it
    // is interface state, dropped when a session opens, like automation
    // lanes. A range selected on a lane is the lane selection, apart from
    // the selection every other edit acts on, so Cut or Clear never reach
    // the comp through it. Copy to Main Playlist (a button on the lane, a
    // key) swipes it onto the active playlist as one compound, one undo
    // step; with Quick Swipe on, the gesture does it as the button comes
    // up and the joins get a short crossfade, as Logic's does.
    bool comp_lanes_shown(int track) const;
    bool toggle_comp_lanes(int track, std::string& why);
    // The track's lane playlists in their order (ids), and their names.
    std::vector<model::Id> take_lane_ids(int track) const;
    std::vector<std::string> take_lane_names(int track) const;
    // Which lane of the active playlist's regions each was swiped from:
    // the lane's index in take_lane_ids(), -1 for none (model::comp_origin).
    int comp_origin_lane(int track, const model::Region& r) const;
    struct LaneSelection {
        int track = -1;          // display index
        model::Id lane = 0;      // the playlist
        model::Frames start = 0, end = 0;
        bool valid() const { return lane != 0 && end > start; }
    };
    // The lane range selected, or one with valid() false: it also lapses
    // when its playlist is the active one now or is gone.
    LaneSelection lane_selection() const;
    bool select_lane_range(int track, model::Id lane, model::Frames a, model::Frames b,
                           std::string& why);
    // A lane's region selected whole: the swipe a click makes.
    bool select_lane_region(int track, model::Id lane, model::Id region, std::string& why);
    bool quick_swipe() const { return quick_swipe_; }
    void set_quick_swipe(bool on) { quick_swipe_ = on; }
    // Copy Selection to Main Playlist. True with `why` set when the swipe went
    // ahead but not all of it as asked: the joins that stayed hard cuts.
    bool comp_copy_to_main(std::string& why);
    // Pro Tools' Duplicate Playlist for comping, or Logic's new comp: a new
    // empty playlist named "<active> comp" (numbered when taken) becomes the
    // active one, so what was active is a take lane, and the lanes open.
    // One undo step.
    bool comp_new(int track, std::string& why);
    // Consolidate's request: the track and the range of the selection (the
    // selected region's span, or the range on the selected track), for
    // `session.consolidate`; refused with a reason when there is none.
    struct ConsolidateRequest {
        model::Id track = 0;
        model::Id playlist = 0;
        model::Frames start = 0, end = 0;
        std::string name;
    };
    bool consolidate_request(ConsolidateRequest& out, std::string& why) const;

    // --- VCA masters and folders (ADR-0048) ---
    // A VCA of `count` tracks from `first` down (the selected tracks): one
    // SetVca, one undo step. `name` empty takes "VCA N". A track may be on
    // several VCAs.
    bool vca_new(int first, int count, const std::string& name, std::string& why);
    // The VCA at `vca` (an index into snapshot().edit->vcas) holds the
    // selected tracks instead of what it held.
    bool vca_assign(int vca, int first, int count, std::string& why);
    bool vca_remove(int vca, std::string& why);
    // Not undoable: the VCA's fader and mute.
    bool set_vca_gain(int vca, float db, std::string& why);
    bool toggle_vca_mute(int vca, std::string& why);
    // A folder of the selected tracks. A track already in one is refused by
    // name, since a track is in one folder at most.
    bool folder_new(int first, int count, const std::string& name, std::string& why);
    // The folder `track` is in goes; the tracks stay where they are.
    bool folder_dissolve(int track, std::string& why);
    // Mute (or solo) every member of `track`'s folder, or release them when
    // all were already: one Compound of SetMix.
    bool folder_toggle(int track, bool solo, std::string& why);
    // The index in snapshot().edit->folders of the folder `track` is in, or -1.
    int folder_index(int track) const;
    // --- mix and edit groups (ADR-0041) ---
    // Group `count` tracks from `first` down (the selected tracks): one
    // SetGroup, one undo step. `name` empty takes "Group N". `attrs` is
    // model::kGroup* bits; a track already in a group is refused by name,
    // since a track is in one group at most.
    bool group_tracks(int first, int count, const std::string& name, std::uint8_t attrs,
                      std::string& why);
    // The group `track` is in goes; the tracks and their mix stay.
    bool dissolve_group(int track, std::string& why);
    // Pro Tools' suspend: the group stays and links nothing, or links again.
    bool toggle_group_suspend(int track, std::string& why);
    // Relative or Absolute volume for `track`'s group.
    bool set_group_relative(int track, bool relative, std::string& why);
    // Link or unlink one attribute (model::kGroup*) on `track`'s group.
    bool toggle_group_attribute(int track, std::uint8_t attr, std::string& why);
    // The index in snapshot().edit->groups of the group `track` is in, or -1.
    int group_index(int track) const;

    // --- mix: not undoable, but still operations ---
    bool set_gain(int track, float db, std::string& why);
    bool set_pan(int track, float pan, std::string& why);
    bool toggle_mute(int track, std::string& why);
    bool toggle_solo(int track, std::string& why);
    // Solo safe (TrackMix::solo_safe): another track's solo never silences
    // this one. Its own solo and mute are left as they are.
    bool toggle_solo_safe(int track, std::string& why);
    // A track's automation mode (ADR-0027), the two trim modes included
    // (§8 slice 5).
    bool set_automation_mode(int track, model::AutomationMode mode, std::string& why);
    // Pro Tools' TrackInput: Input mode (the live input whatever the
    // transport does) or back to Auto. Off, which only a file can set, is
    // not Input, so it goes to Input. Refused on a MIDI or instrument
    // track, whose engine track carries no audio of its own (ADR-0014 M-1).
    bool toggle_input_monitor(int track, std::string& why);
    // TrackPunch (TrackMix::track_punch, ADR-0043): whether the armed track
    // records in punch takes. Refused on a MIDI or instrument track.
    bool toggle_track_punch(int track, std::string& why);
    // Off, Read, Touch, Latch, Write, Trim (Touch/Trim), Write/Trim and
    // round to Off: the selector's order. Pro Tools' own order is unverified
    // (brain/research/protools-logic-key-bindings.md has no automation modes).
    bool step_automation_mode(int track, std::string& why);
    bool set_bus_gain(float db, std::string& why);

    // The end of the last region, or one minute when the session is empty.
    model::Frames session_end() const;
    // The playlist and region under `frame` on display track `track`.
    const model::Region* region_at(int track, model::Frames frame) const;

private:
    bool submit(const model::Operation& op, std::string& why);
    // The ids of `count` tracks from `first` down.
    bool selected_ids(int first, int count, std::vector<model::Id>& out, std::string& why) const;
    bool refuse_in_mode(const char* what, std::string& why) const;
    model::Frames snapped(model::Frames f) const;
    void clamp_scroll();
    // Pages the view so `frame` is a tenth in from the left edge, when it
    // is off screen (as follow() pages).
    void reveal(model::Frames frame);
    const model::TrackEdit* track(int index) const;
    // `m` as one SetMix, or with the members `m`'s group links as one
    // Compound of SetMix: one revision, so the group moves together.
    bool submit_mix(const model::TrackMix& m, std::string& why);
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
    // Refuses, by name, a gesture whose Shuffle form is not built (Strip
    // Silence); the ones that are build their ripple (shuffle_shift).
    bool shift_time(bool insert, std::string& why);
    bool refuse_shuffle(const char* gesture, const char* nothing_was, std::string& why) const;
    // Shuffle moves regions, not the automation under them: names the tracks
    // whose lanes now sit out of step, as a held notice.
    void note_shuffle_automation(const char* gesture, int first, int count);
    // One gesture's members as one Compound labelled `gesture`, refused
    // unsent past the member limit.
    bool submit_compound(std::vector<model::Member> members, const char* gesture,
                         const char* nothing_was, std::string& why);
    // `op` alone, or with `follow` (Automation Follows Edit) as one
    // Compound labelled `gesture`, so one undo step.
    bool submit_following(model::Member op, std::vector<model::EditAutomation> follow,
                          const char* gesture, const char* nothing_was, std::string& why);
    // follow_notice_ for `lanes` (lane_labels), once `gesture` went through.
    void note_left_behind(const char* gesture, const std::vector<std::string>& lanes);
    // A region move, its lanes following when Automation Follows Edit is on.
    bool submit_move(const model::MoveRegion& op, std::string& why);
    // The moves that carry a clip group with the clip `op` moves; false when
    // the clip is in no group.
    bool group_move_ops(const model::MoveRegion& op, std::vector<model::Member>& out) const;

    TimelineDocument& doc_;
    void refresh_rows() const;
    const RowPolicy* rows_ = nullptr;
    bool autolane_selected() const { return sel_.autolane >= 0; }
    // Cut, Copy, Paste and Clear on the selected lane's range: that lane on
    // every track the range covers that shows its lanes; the others are
    // appended to `hidden` by name. Several lanes' edits go as one Compound.
    std::vector<int> lane_range_tracks(std::vector<std::string>& hidden) const;
    // "Audio 1's Volume lane" on one track, "the selected Volume lanes" on several.
    std::string lane_range_label() const;
    bool submit_lane_ops(std::vector<model::EditAutomation> ops, const char* gesture,
                         const char* nothing_was, std::string& why);
    // follow_notice_ for the tracks `gesture` left alone, its lanes hidden.
    void note_lanes_hidden(const char* gesture, const std::vector<std::string>& tracks);
    // After an undo or redo step.
    void after_history_step();
    bool automation_copy(bool cut, std::string& why);
    bool automation_paste(std::string& why);
    bool automation_clear(std::string& why);
    bool press_takelane(const Hit& h, double x, std::string& why);
    bool release_swipe(const Drag& d, std::string& why);
    bool press_autolane(const Hit& h, double x, double y, std::string& why);
    bool release_autolane(const Drag& d, std::string& why);
    void move_autolane(double x, double y);
    // The Pencil on a clip's waveform (edit_gestures.cpp).
    bool press_pencil(const Hit& h, double x, double y, std::string& why);
    float pencil_level(const Drag& d, double y) const;
    bool release_pencil(const Drag& d, std::string& why);
    // The lane value under pixel `y` of lane `lane` of track `index`.
    float autolane_value_at(int index, int lane, double y) const;

    mutable ViewState view_;
    ViewState saved_view_;
    bool has_saved_view_ = false;
    EditMode mode_ = EditMode::Slip;
    bool automation_follows_edit_ = true;
    std::string follow_notice_;
    Tool tool_ = Tool::Smart;
    GridValue grid_ = GridValue::Beat;
    Selection sel_;
    struct TempoStep {
        std::vector<model::TempoEntry> before, after;
    };
    bool submit_tempo(std::vector<model::TempoEntry> next, std::string& why);
    bool tempo_step(bool undo, std::string& why);
    std::vector<TempoStep> tempo_undo_, tempo_redo_;
    Drag drag_;
    model::FadeShape fade_shape_ = model::FadeShape::Linear;
    model::FadeShape crossfade_shape_ = model::FadeShape::EqualPower;
    Clipboard clipboard_;
    std::vector<AutomationClip> auto_clips_;
    std::set<model::Id> autolanes_open_;
    std::set<model::Id> comp_lanes_open_;
    LaneSelection lane_sel_;
    bool quick_swipe_ = false;
    int event_lane_ = 0;
    int repeat_count_ = 1;
    int loop_count_ = 3;
    void apply_marker_view(model::Id marker);
    void prune_marker_views();
    Locator locate_;
    MarkerViewStore* marker_views_ = nullptr;
    TrackHider hide_tracks_;
    uint64_t clipboard_serial_ = 0;
    model::Id spot_request_ = kNoId;
    const SampleSource* samples_ = nullptr;
    SampleEditSender sample_sender_;
    bool recording_ = false;
};

// The audio playlist `id` of `t`, or null.
const model::Playlist* audio_playlist(const model::TrackEdit& t, model::Id id);
// A track's take lanes: its playlists other than the active one, in their
// order, at most kMaxTakeLanes; none on a MIDI track.
constexpr std::size_t kMaxTakeLanes = 32;
std::vector<model::Id> take_lanes_of(const model::TrackEdit& t);

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
