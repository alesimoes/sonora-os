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

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "time_format.h"
#include "timeline_document.h"

namespace sonora::ui {

enum class EditMode { Shuffle, Slip, Spot, Grid };
enum class Tool { Smart, Trim, Select, Grab };

const char* edit_mode_name(EditMode m);
const char* tool_name(Tool t);

struct ViewState {
    double fpp = 1200.0;    // frames per pixel: 40 px a second at 48 kHz
    double scroll = 0.0;    // timeline frame at the left edge
    double scroll_y = 0.0;  // pixels of track lanes above the top edge
    double width = 0.0;     // the track area, in pixels
    double height = 0.0;
    double track_height = 72.0;
    float wave_scale = 1.0f;  // vertical zoom of the waveforms

    double x_of(model::Frames f) const { return (static_cast<double>(f) - scroll) / fpp; }
    model::Frames frame_at(double x) const;
    double y_of_track(int index) const { return index * track_height - scroll_y; }
    int track_at(double y) const;  // -1 above or below the lanes is the caller's check
};

struct Selection {
    int track = -1;              // display index; -1 is none
    model::Id region = kNoId;
    model::Frames insertion = 0;   // the edit cursor
    model::Frames start = 0, end = 0;  // a time range on `track`; empty when equal

    bool has_range() const { return end > start; }
};

enum class HitZone { None, Lane, Body, Select, TrimStart, TrimEnd };

struct Hit {
    int track = -1;
    model::Id region = kNoId;
    HitZone zone = HitZone::None;
};

// A gesture in progress: interface state, drawn outside the model until the
// release turns it into one operation (timeline-model.md).
struct Drag {
    enum class Kind { None, Move, TrimStart, TrimEnd, Range };
    Kind kind = Kind::None;
    int track = -1;
    model::Id region = kNoId;
    model::Frames press_frame = 0;
    double press_x = 0;
    model::Frames orig_start = 0, orig_end = 0;
    model::Frames start = 0, end = 0;  // the preview
    bool moved = false;
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
    const ViewState& view() const { return view_; }
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
    void clear_selection();

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
    bool clear(std::string& why);
    bool nudge(int direction, std::string& why);
    bool undo(std::string& why);
    bool redo(std::string& why);

    // --- mix: not undoable, but still operations ---
    bool set_gain(int track, float db, std::string& why);
    bool set_pan(int track, float pan, std::string& why);
    bool toggle_mute(int track, std::string& why);
    bool toggle_solo(int track, std::string& why);
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
    const model::TrackEdit* track(int index) const;
    // The region a command acts on: the selected one, else the one under
    // the insertion point on the selected track.
    const model::Region* target_region(int& track_index) const;

    TimelineDocument& doc_;
    ViewState view_;
    ViewState saved_view_;
    bool has_saved_view_ = false;
    EditMode mode_ = EditMode::Slip;
    Tool tool_ = Tool::Smart;
    GridValue grid_ = GridValue::Beat;
    Selection sel_;
    Drag drag_;
};

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
