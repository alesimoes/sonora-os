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
// real common/model Document. A synthetic signal per channel feeds the
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
#include <memory>
#include <string>
#include <vector>

#include "sonora/api/engine_control.h"
#include "sonora/api/state_block.h"
#include "sonora/api/state_view.h"

namespace sonora::model {
class Document;
}

namespace sonora::api::testing {

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
    };

    FakeEngine();
    explicit FakeEngine(Config config);
    ~FakeEngine() override;

    bool connect(std::string& error) override;
    const Hello& hello() const override { return hello_; }
    uint64_t send(const CommandRequest& request) override;
    uint64_t send(const ParameterChange& change) override;
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

    Config cfg_;
    Hello hello_;
    StateBlockMemory block_;
    StateWriter writer_;
    StateView view_;
    std::unique_ptr<model::Document> document_;

    std::deque<std::string> inbound_;
    std::deque<std::string> outbound_;
    uint64_t seq_ = 0;
    bool connected_ = false;
    bool dead_ = false;
    uint64_t death_ns_ = UINT64_MAX;

    TransportState mode_ = TransportState::Stopped;
    uint64_t record_start_ns_ = 0;
    int32_t take_ = 1;
    std::vector<bool> armed_;
    std::vector<float> gain_;
    std::vector<float> scripted_;
    std::vector<uint32_t> clips_;
    uint32_t health_ = 0;
    uint32_t xruns_ = 0;
    uint32_t clock_steps_ = 0;
    uint64_t start_ns_ = 0;
    uint64_t last_publish_ns_ = 0;
};

}  // namespace sonora::api::testing
