// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Pitch editor's state (time.pitch, ADR-0143): the notes found in one
// region, the operator's edit of each, and Apply, which turns them into one
// sample.edit "pitch" through pitch.h. Qt-free, so all of it is tested
// without a display; the panel only draws what this says and reports the
// operator's gestures back.
//
// Nothing here reaches the session until Apply, and Apply is refused, never
// guessed: no shifter in the engine, an edit already on its way, notes that
// no longer describe the audio (the region was rendered: analyse the copy
// again or undo). Every refusal is also kept as the held notice, which stays
// until the operator dismisses it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "sonora/pitch/plan.h"
#include "sonora/pitch/segment.h"
#include "sonora/pitch/tracker.h"
#include "sonora/ui_pitch/pitch_engine.h"

namespace sonora::ui_pitch {

enum class SnapMode { Off, Semitone, Scale };

// The region the notes were found in.
struct PitchRegion {
    uint64_t id = 0;
    uint64_t position = 0;  // timeline frame of its first sample
    uint64_t length = 0;    // frames
    uint32_t rate = 0;
    std::string name;
};

class PitchEditorModel {
public:
    explicit PitchEditorModel(PitchCommandSink* sink = nullptr) : sink_(sink) {}

    void set_sink(PitchCommandSink* sink) { sink_ = sink; }
    void set_capability(PitchCapability capability) { capability_ = std::move(capability); }
    const PitchCapability& capability() const { return capability_; }

    // A session opened: nothing of the last one's region, edits, notice or
    // pending Apply survives it. The engine's capability stays (it is the
    // connection's, not the session's).
    void reset_session();

    // `notes` are frames from the region's start. Refused (held) when there
    // are none or one runs past the region.
    bool open(const PitchRegion& region, std::vector<pitch::Note> notes, std::string& why);
    // Tracks and segments `mono`, the region's audio; refuses (held) material
    // that is unpitched or polyphonic, as segment_notes does.
    bool analyse(const PitchRegion& region, const std::vector<float>& mono,
                 pitch::PitchTracker& tracker, std::string& why);
    bool is_open() const { return open_; }
    const PitchRegion& region() const { return region_; }

    std::size_t size() const { return plan_.size(); }
    const pitch::Note& note(std::size_t i) const { return plan_.note(i); }
    const pitch::NoteEdit& edit(std::size_t i) const { return plan_.edit(i); }
    // Where note i sounds once edited, in cents from A4.
    double pitch_cents(std::size_t i) const;
    bool edited() const;

    int selected() const { return selected_; }
    // -1 clears the selection; any other index outside the notes is refused.
    bool select(int index);

    // A vertical drag of note `index`: drag_to sets its shift from where the
    // drag began, snapped as snap_mode() says.
    bool begin_drag(int index, std::string& why);
    bool drag_to(double delta_cents, std::string& why);
    void end_drag() { drag_index_ = -1; }
    bool dragging() const { return drag_index_ >= 0; }

    SnapMode snap_mode() const { return snap_mode_; }
    void set_snap_mode(SnapMode mode) { snap_mode_ = mode; }
    int key() const { return key_; }
    bool set_key(int key, std::string& why);
    pitch::Scale scale() const { return scale_; }
    void set_scale(pitch::Scale scale) { scale_ = scale; }
    double amount() const { return amount_; }
    bool set_amount(double percent, std::string& why);
    // Every note toward the nearest degree of key() and scale() by amount().
    bool snap_all(std::string& why);

    // The selected note's inspector.
    bool set_straighten(double percent, std::string& why);  // 0 keeps the drift, 100 removes it
    bool set_vibrato(double percent, std::string& why);     // 100 keeps it, 0 removes, 200 doubles
    bool set_formant(int cents, std::string& why);
    bool set_gain(double db, std::string& why);

    // Builds the spec, sends sample.edit and waits for its reply (on_reply).
    bool apply(std::string& why);
    void on_reply(uint64_t seq, bool ok, const std::string& message);
    bool pending() const { return pending_seq_ != 0; }
    // Rendered: the notes describe audio the region no longer plays.
    bool stale() const { return stale_; }
    const std::string& last_argument() const { return last_argument_; }

    // Every note back to as found. Refused while an Apply is on its way.
    bool reset_edits(std::string& why);

    const std::string& notice() const { return notice_; }
    void dismiss_notice() { notice_.clear(); }
    const std::string& status() const { return status_; }

private:
    bool refuse(std::string& why, const std::string& text);
    bool editable(std::string& why);
    bool selected_note(std::string& why);
    double snapped(double cents) const;

    PitchCommandSink* sink_ = nullptr;
    PitchCapability capability_;
    bool open_ = false;
    PitchRegion region_;
    pitch::PitchPlan plan_{std::vector<pitch::Note>{}};
    int selected_ = -1;
    int drag_index_ = -1;
    double drag_origin_ = 0.0;
    SnapMode snap_mode_ = SnapMode::Semitone;
    int key_ = 0;
    pitch::Scale scale_ = pitch::Scale::Major;
    double amount_ = 100.0;
    uint64_t pending_seq_ = 0;
    bool stale_ = false;
    std::string last_argument_;
    std::string notice_;
    std::string status_;
};

}  // namespace sonora::ui_pitch
