// SPDX-License-Identifier: GPL-3.0-or-later
//
// FakeEngine: the engine as a client sees it, with no audio, no thread and
// no disk (ADR-0007 §7). Owned by the provider (rt-engineer), so it cannot
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
// real common/model Document. Plugins (ADR-0012 §8) on a script: a catalog
// (fake_catalog() unless Config::plugins says otherwise) sent after the
// Snapshot; insert operations accepted when they name a usable catalog
// plugin with the track's channel count; PluginParameterChange checked
// against the catalog's dense index and range; every insert's flags and
// latency in the state block, its chain's latency (one IPC block plus its
// running inserts') on the track, and the largest as the session's
// compensation. No audio passes through a plugin.
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
    // faulted flag, a miss counted) or, with an empty `why`, comes back.
    void script_insert_fault(uint64_t insert, const std::string& why);
    // The last value a PluginParameterChange set, by insert and dense index;
    // NaN when none has.
    float plugin_value(uint64_t insert, uint32_t param) const;

    // The linear level channel `ch` shows at `t_s` seconds: a level of its
    // own per channel, so a meter showing a neighbour's signal is visible.
    // Channel 10 (index 9) goes just over full scale for 50 ms every 7 s.
    static float level_at(uint32_t ch, double t_s);
    // The Engine-domain commands this build's engine implements.
    static const std::vector<std::string>& engine_commands();

private:
    uint64_t now() const;
    bool alive() const;
    void handle(const std::string& bytes);
    bool command(const CommandRequest& r, std::string& why);
    void reply(uint64_t seq, DispatchResult result, std::string message);
    void broadcast(const ModelUpdate& u);
    // Frames, both ways, as the socket will carry them (api/wire.h).
    static void queue(std::deque<std::string>& q, const std::string& message);
    static std::string take_file(int32_t take);
    void check_take();
    uint64_t playhead() const;
    void send_catalog();
    bool plugin_change(const PluginParameterChange& c, std::string& why);
    // PluginFault for every insert whose plugin is not in the catalog.
    void report_missing();
    void insert_fields(TrackFields* tracks, uint32_t count, TransportFields& tf) const;

    Config cfg_;
    Hello hello_;
    StateBlockMemory block_;
    StateWriter writer_;
    StateView view_;
    std::unique_ptr<model::Document> document_;

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
    std::vector<float> gain_;
    std::vector<float> scripted_;
    std::vector<uint32_t> clips_;
    uint32_t health_ = 0;
    bool fail_takes_ = false;
    int32_t failed_take_ = 0;     // the take TakeFailed was said for
    bool take_failed_shown_ = false;
    bool inputs_connected_ = true;
    uint32_t xruns_ = 0;
    uint32_t clock_steps_ = 0;
    uint64_t start_ns_ = 0;
    uint64_t last_publish_ns_ = 0;

    uint64_t catalog_generation_ = 1;
    std::map<std::string, uint32_t> plugin_latency_;   // by catalog id
    std::map<uint64_t, std::string> faults_;           // by insert id
    std::map<uint64_t, uint32_t> misses_;              // by insert id
    std::map<uint64_t, std::vector<float>> plugin_values_;
};

}  // namespace sonora::api::testing
