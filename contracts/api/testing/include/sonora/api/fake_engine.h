// SPDX-License-Identifier: GPL-3.0-or-later
//
// FakeEngine: the engine as a client sees it, with no audio, no thread and
// no disk but File > Save's (ADR-0007 §7). Owned by the provider
// (rt-engineer), so it cannot
// drift into agreeing with whatever a consumer believes: it is held to the
// same conformance suite as InProcessEngine (contracts/api/conformance),
// and engine/control's test checks that both implement the same commands.
//
// Deterministic. Requests queue in send() and are handled in the next
// poll(), which then publishes the state block once; time comes from an
// injectable clock. Every message crosses the wire codec, as in the real
// one, and the state block is written by the same StateWriter.
//
// What it does: the Engine-domain commands engine/control installs, with
// the same refusals in the same words; ParamId::Gain; the model path on a
// real common/model Document, whose faders (gain, mute) it publishes when
// an update changes them, on a Resync and when it starts (connect), as the
// real one does; a track past the document's end keeps both.
// Plugins (ADR-0012 §8) on a script: a catalog
// (fake_catalog() unless Config::plugins says otherwise) sent after the
// Snapshot; insert operations accepted when they name a usable catalog
// plugin with the track's channel count; PluginParameterChange checked
// against the catalog's key and range; every insert's flags and latency in
// the state block, its chain's latency (one IPC block plus its running
// inserts') on the track, and the largest as the session's compensation;
// one standing PluginFault per active insert that is down or missing,
// cleared when it runs again or leaves the chain; values moved in a
// plugin's own editor recorded as SetInsertParams at gesture end and on
// close; FlushPluginState applying the state a plugin was scripted to
// have. No audio passes through a plugin.
//
// ModelAction::Save writes the session file and each MIDI sequence file it
// names that is not there yet, from the fake's document, through a
// temporary file and a rename, as the engine does, but syncs nothing, keeps
// no pre-migration copy and never cleans up: those are the engine's saver's
// (engine/control session_saver.h), held to its own tests.
//
// The transport rolls on a script: play starts
// the playhead from where it stands at the sample rate, stop holds it,
// return to zero puts it at 0, and a take starts at the playhead, as the
// real one's does. There is no residency gate: everything is resident. A synthetic signal per channel feeds the
// meters (on by default). Scripting hooks let a UI test show engine death,
// a notice, an xrun or a fatal backend.
//
// What it is not: a place to design engine behaviour. A command the UI
// wants and the engine lacks is a request to the rt-engineer, then a change
// to both, in one PR.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "sonora/api/engine_control.h"
#include "sonora/api/state_block.h"
#include "sonora/api/state_view.h"
#include "sonora/api/wire.h"

namespace sonora::model {
class Document;
class LoadParts;
struct ReplicationUpdate;
struct Snapshot;
}

namespace sonora::api::testing {

// A small catalog to develop insert slots against: a mono LV2 gain, a
// stereo LV2 limiter, a mono VST3 compressor with an editor, and an LV2
// plugin the scan refused.
std::vector<PluginInfo> fake_catalog();

class FakeEngine final : public EngineControl {
public:
    using Clock = std::function<uint64_t()>;   // monotonic nanoseconds

    struct Config {
        uint32_t tracks = 18;   // the UMC1820's inputs, as the engine defaults to
        uint32_t sample_rate = 48000;
        uint32_t buffer_frames = 128;
        uint64_t publish_period_ns = 10'000'000;
        // What Hello announces: set either to script a version mismatch.
        uint32_t protocol_version = kProtocolVersion;
        uint32_t state_layout_version = kStateLayoutVersion;
        bool signal = true;
        Clock clock;   // empty: std::chrono::steady_clock
        std::vector<PluginInfo> plugins = fake_catalog();
        // Engine commands a later engine is to implement (planning/BACKLOG.md
        // Track 1), announced in Hello so an interface can be wired to them
        // before they land. The fake runs none of them: each is answered Ok
        // and recorded in planned_log(), so a test sees the command arrive
        // over the wire and nothing more. Interface commands and ids not in
        // the catalog are not announced. Empty by default, so --fake-engine
        // never claims a command nothing runs.
        std::vector<std::string> planned_commands;
        // Hello's capabilities. The fake parses a play range as the engine
        // does; clear this to stand in for an engine that predates it.
        std::vector<std::string> capabilities = {kCapabilityPlayRange};
        // The engine's midi_max_entries (ADR-0015 §5), so a test can make a
        // Load the engine refuses without 2^24 notes.
        uint64_t midi_max_entries = uint64_t{1} << 24;
        // The session folder it announces at connect (ADR-0018 §4), an
        // absolute path. Empty: none until a Load names its file, and an
        // interface then saves where it was told to ($SONORA_SESSIONS_DIR).
        // The fake writes no audio there: it remembers which folder each
        // take was recorded in (take_folder()).
        std::string folder;
    };

    FakeEngine();
    explicit FakeEngine(Config config);
    ~FakeEngine() override;

    bool connect(std::string& error) override;
    const Hello& hello() const override { return hello_; }
    uint64_t send(const CommandRequest& request) override;
    uint64_t send(const ParameterChange& change) override;
    uint64_t send(const PluginParameterChange& change) override;
    uint64_t submit(const ModelRequest& request) override;
    void poll(Inbox& inbox) override;
    StateView& state() override { return view_; }

    // --- scripting ---------------------------------------------------------
    // The engine stops answering: no reply, no event, no publish, so the
    // heartbeat stands still and StateView::engine_alive() goes false.
    void die() { dead_ = true; }
    // The same, from monotonic time `ns` on.
    void die_at(uint64_t ns) { death_ns_ = ns; }
    void post_event(const EngineEvent& event);
    void set_health_flags(uint32_t flags) { health_ = flags; }
    void add_xruns(uint32_t n) { xruns_ += n; }
    void add_clock_steps(uint32_t n) { clock_steps_ += n; }
    // A fixed peak for a track from the next publish on, instead of the
    // synthetic signal; negative goes back to the signal.
    void script_peak(uint32_t track, float peak);
    // One publish now, without waiting for poll().
    void publish();
    // While set, a take fails to write as a full disk would: the engine
    // says TakeFailed and raises the health flag at its next poll, and
    // stop reports the take as Failed. A later take that records cleanly
    // clears both, as the real engine's control thread does.
    void script_take_failure(bool fail) { fail_takes_ = fail; }
    // While not empty, every Save is refused with `why` and writes nothing,
    // as one the disk refused would be.
    void script_save_failure(std::string why) { save_failure_ = std::move(why); }
    // Saves written since construction.
    uint32_t saves() const { return saves_; }
    // The capture inputs go away (`why` says how) or come back: an
    // InputsMissing event, the health flag and the per-track flags, as the
    // engine's housekeeping reports them.
    void script_inputs(bool connected, const std::string& why = "");
    // A new scan found `plugins`: the catalog is replaced, its generation
    // bumped and sent, as after plugin.rescan. Inserts whose plugin is gone
    // show missing.
    void script_catalog(std::vector<PluginInfo> plugins);
    // The latency a plugin reports, by catalog id, for every insert of it.
    void script_plugin_latency(const std::string& plugin_id, uint32_t frames);
    // An insert's host goes down (`why` says how: a PluginFault event, the
    // faulted flag, a miss counted) or, with an empty `why`, comes back (the
    // fault cleared).
    void script_insert_fault(uint64_t insert, const std::string& why);
    // The operator moves a parameter in the plugin's own editor: the host's
    // ParamTouched. Nothing reaches a client until the gesture ends, or the
    // editor closes; then the engine applies SetInsertParams, as the real
    // one will (messages.h).
    void script_editor_change(uint64_t insert, const std::string& key, float value);
    void script_editor_gesture_end(uint64_t insert);
    void script_editor_closed(uint64_t insert);
    // The state GetState would return for an insert now, as the file it
    // would be written to: FlushPluginState records it with SetInsertState.
    void script_plugin_state(uint64_t insert, const std::string& sha256, uint64_t bytes);
    // The plugin's live value, by insert and key, as a PluginParameterChange
    // or its own editor last set it; NaN when neither has.
    float plugin_value(uint64_t insert, const std::string& key) const;

    // A take file in the session's Audio Files that no source names, as
    // its header describes it (session-format.md, "Crash recovery"). The
    // fake has no disk: it offers what is scripted, at connect and at each
    // Load of the session it is of (by `session_id`, or with none by
    // `session`'s name, never "Untitled", as the engine's
    // take_is_of_session()) with no source at `path` and not in the
    // document's left_takes, as a RecoveryOffer, one line per take;
    // session.place_recovered puts each on the track `track_id` names at
    // `position`, `frames` long, as the engine places a whole take, one
    // undo step per take (a punch or loop take's placement is the
    // engine's, held to its own tests), or into the pool when that track
    // is gone; session.leave_recovered withdraws it and adds its paths to
    // the document's left_takes.
    struct RecoveredTake {
        std::string path;   // "Audio Files/<name>"
        std::string session = "Untitled";
        std::string session_id;
        int32_t take = 1;
        uint64_t track_id = 0;
        uint64_t position = 0;
        uint64_t frames = 0;
        uint16_t channels = 1;
    };
    void script_recovered_take(RecoveredTake take) { recovered_.push_back(std::move(take)); }

    // The linear level channel `ch` shows at `t_s` seconds: a level of its
    // own per channel, so a meter showing a neighbour's signal is visible.
    // Channel 10 (index 9) goes just over full scale for 50 ms every 7 s.
    static float level_at(uint32_t ch, double t_s);
    // The Engine-domain commands this build's engine implements.
    static const std::vector<std::string>& engine_commands();
    // Every Config::planned_commands request received, as {command, target}.
    struct PlannedCall {
        std::string command;
        int32_t target;
    };
    const std::vector<PlannedCall>& planned_log() const { return planned_log_; }
    // The engine's own document, read-only: what a test checks a mix field
    // the state block does not carry (solo, solo safe) against, rather than
    // the interface's replica of it.
    const model::Snapshot& document() const;
    // The session folder take `take` was recorded in: the one active when
    // it started. Empty for a take not recorded, or recorded with no
    // folder announced.
    std::string take_folder(int32_t take) const;

private:
    uint64_t now() const;
    bool alive() const;
    void handle(const std::string& bytes);
    bool command(const CommandRequest& r, std::string& why);
    void reply(uint64_t seq, DispatchResult result, std::string message);
    void broadcast(const ModelUpdate& u);
    // A model update, a Snapshot as its header, parts and window.
    void broadcast(const model::ReplicationUpdate& u);
    // Frames, both ways, as the socket will carry them (api/wire.h).
    static void queue(std::deque<std::string>& q, const std::string& message);
    static std::string take_file(int32_t take);
    void check_take();
    uint64_t playhead() const;
    void send_catalog();
    bool plugin_change(const PluginParameterChange& c, std::string& why);
    // One standing PluginFault per active insert that is down or missing,
    // and a clear for each that no longer is; with `reset` (another session
    // opened), every standing one is cleared first.
    void refresh_faults(bool reset);
    // SetInsertParams for what the insert's editor moved and the document
    // does not hold yet.
    void record_editor_values(uint64_t insert);
    bool flush_plugin_state(const std::string& which, std::string& why);
    // ModelAction::Save: true once the session file is in place.
    bool save(const std::string& op, std::string& why);
    // An operation the engine makes itself (op_codec bytes), applied and
    // sent to the replica.
    bool apply_own(const std::string& op, std::string& why);
    void insert_fields(TrackFields* tracks, uint32_t count, TransportFields& tf) const;
    // As engine/control's PlaybackController::apply_mix: the document's
    // faders onto the published tracks, track i being the document's i-th.
    void apply_mix();

    Config cfg_;
    Hello hello_;
    StateBlockMemory block_;
    StateWriter writer_;
    StateView view_;
    std::unique_ptr<model::Document> document_;
    // A Load in parts (protocol 10): the parts so far, and why one was
    // refused, which the Load that follows is refused with.
    std::unique_ptr<model::LoadParts> load_parts_;
    std::string load_refused_;

    std::deque<std::string> inbound_;    // frames
    std::deque<std::string> outbound_;   // frames
    wire::Reassembler engine_side_;
    wire::Reassembler client_side_;
    uint64_t seq_ = 0;
    bool connected_ = false;
    bool dead_ = false;
    uint64_t death_ns_ = UINT64_MAX;

    TransportState mode_ = TransportState::Stopped;
    // Where the playhead was when the transport last started or stopped,
    // and when that was.
    uint64_t position_ = 0;
    uint64_t moved_ns_ = 0;
    int32_t take_ = 1;
    std::vector<bool> armed_;
    std::vector<std::string> planned_;
    std::vector<PlannedCall> planned_log_;
    std::vector<float> gain_;
    std::vector<bool> muted_;
    // In Input mode: the document's, on an audio track only, as the real
    // engine's PlaybackController::apply_monitoring.
    std::vector<bool> input_monitor_;
    // Engine tracks a MIDI or instrument track sits on: no audio reaches
    // them in this version, as on the real engine (ADR-0014 M-1).
    std::vector<bool> silent_;
    uint32_t silent_said_ = 0;
    std::vector<float> scripted_;
    std::vector<uint32_t> clips_;
    uint32_t health_ = 0;
    // TransportFlag: the click and count-off toggles. The fake counts off
    // nothing; a take starts at once.
    uint32_t transport_flags_ = 0;
    bool fail_takes_ = false;
    std::string save_failure_;
    uint32_t saves_ = 0;
    int32_t failed_take_ = 0;     // the take TakeFailed was said for
    bool take_failed_shown_ = false;
    bool inputs_connected_ = true;
    uint32_t xruns_ = 0;
    uint32_t clock_steps_ = 0;
    uint64_t start_ns_ = 0;
    uint64_t last_publish_ns_ = 0;

    uint64_t catalog_generation_ = 1;
    std::map<std::string, uint32_t> plugin_latency_;   // by catalog id
    std::map<uint64_t, std::string> faults_;           // scripted, by insert id
    std::map<uint64_t, std::string> said_faults_;      // standing, as sent
    std::map<uint64_t, uint32_t> misses_;              // by insert id
    std::map<uint64_t, std::map<std::string, float>> plugin_values_;
    // Moved in an editor, not yet in the document.
    std::map<uint64_t, std::map<std::string, float>> editor_moved_;
    struct ScriptedState {
        std::string sha256;
        uint64_t bytes = 0;
    };
    std::map<uint64_t, ScriptedState> plugin_state_;
    // Scripted orphans, and the ones the standing offer holds.
    std::vector<RecoveredTake> recovered_;
    std::vector<RecoveredTake> offered_;
    bool offer_standing_ = false;
    void offer_recovered();
    void withdraw_recovered();
    bool place_recovered(std::string& why);
    // ModelAction::Load, as engine/control's: false, with `why`, and the
    // session as it was.
    bool load(const std::string& op, std::string& why);
    // The active session folder, and the folder of every take recorded.
    std::string folder_;
    std::map<int32_t, std::string> take_folders_;
    void announce_folder();
};

}  // namespace sonora::api::testing
