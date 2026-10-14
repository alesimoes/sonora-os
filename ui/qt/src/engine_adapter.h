// SPDX-License-Identifier: GPL-3.0-or-later
//
// THE SEAM. Everything the Qt interface knows about the engine passes through
// this one interface. Since M4 its one implementation is ControlAdapter
// (control_adapter.h), which forwards every call to ADR-0007 §4.1's
// api::EngineControl and api::StateView:
//
//   here                        is
//   ------------------------    -------------------------------------------
//   EngineInfo                  api::Hello
//   EngineAdapter::connect      EngineControl::connect (version check)
//   EngineAdapter::send         EngineControl::send(CommandRequest)
//   EngineAdapter::poll         EngineControl::poll (replies)
//   EngineAdapter::take_events  EngineControl::poll (events, kept until taken)
//   EngineAdapter::frame        StateView::frame      (one publish)
//   EngineAdapter::take_peak    StateView::take_peak  (per-reader cursor)
//   EngineAdapter::engine_alive StateView::engine_alive (heartbeat)
//   EngineAdapter::send_track_gain
//                               EngineControl::send(ParameterChange), Gain
//   EngineAdapter::send_plugin_parameter
//                               EngineControl::send(PluginParameterChange)
//   EngineAdapter::take_catalog EngineControl::poll (PluginCatalog, the latest)
//
// It stays as a narrow seam rather than handing api::EngineControl to the
// shell, so the shell's tests can record routing (tests/recording_adapter.h)
// without an engine of any kind.
//
// Rules:
//
//   - Nothing in ui/qt outside ShellModel and main.cpp holds an
//     EngineAdapter. The meters, the key router and QML never see it.
//   - No type here carries a Qt type.
//   - Nothing may be added here that §4.1 does not have. A field the UI
//     wants and §4.1 lacks is a contract request to the rt-engineer (§6),
//     not a line in this file.
//
// Nothing here is reachable from the audio thread: it is another process.
// Every call is made from the GUI thread and must not block it.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "sonora/api/messages.h"
#include "sonora/command.h"

namespace sonora::ui {

// api::Hello, less the version fields: there is no wire here to mismatch.
struct EngineInfo {
    uint32_t sample_rate = 0;
    uint32_t buffer_frames = 0;
    uint32_t track_count = 0;
    // What the engine has handlers for. Feeds KeyMap::ensure_essentials()
    // together with the interface's own registry, as §5 says. Read again
    // from each new connection's Hello, as a restarted engine may be another
    // build; ShellModel hands a changed list to the key router.
    // The format and track count stay those of the first Hello: what is
    // sized from them is not resized on a reconnect.
    std::vector<std::string> implemented_commands;
};

// The engine's TransportState, spelled again because ui/ may not include
// engine/ (ADR-0007 §1). §4.1's TransportFields carries the same three.
enum class TransportMode { Stopped, Rolling, Recording };

// Health flags from §4.1's EngineStateBlock::health_flags.
enum HealthFlag : uint32_t {
    kHealthServerGone = 1u << 0,
    kHealthNoInputs = 1u << 1,
    kHealthTakeFailed = 1u << 2,
    // Layout 2: this engine hosts no plugin, so every insert is down.
    kHealthPluginsDisabled = 1u << 3,
};

// api::InsertFlag, spelled again: one insert slot of a track as the state
// block's layout 2 publishes it.
enum InsertSlotFlag : uint32_t {
    kSlotPresent = 1u << 0,
    kSlotActive = 1u << 1,
    kSlotBypassed = 1u << 2,
    kSlotRunning = 1u << 3,
    kSlotRestarting = 1u << 4,
    kSlotFaulted = 1u << 5,
    kSlotMissing = 1u << 6,
};

// Pro Tools' A to J. api::kInsertSlots and model::kInsertSlots are the same
// number; control_adapter.cpp asserts it.
constexpr uint32_t kInsertSlotCount = 10;

struct InsertSlotSnapshot {
    uint32_t flags = 0;            // InsertSlotFlag
    uint32_t latency_frames = 0;   // the insert's own, as its host reports it
};

struct TransportSnapshot {
    TransportMode state = TransportMode::Stopped;
    uint64_t playhead_frames = 0;
    int32_t take = 0;          // the take that is open, or the next one
    uint32_t xruns = 0;
    uint32_t health_flags = 0;
    // Layout 2: the delay compensation the playing plan applies, and how far
    // short of what the inserts need it falls when clamped at its maximum.
    uint32_t compensation_frames = 0;
    uint32_t compensation_shortfall_frames = 0;
    // Update 2026-10-12 item 3: a seqlock read that ran out of retries. The
    // interface shows it as stale for that tick rather than spinning.
    bool stale = false;
};

struct TrackSnapshot {
    bool armed = false;
    bool muted = false;
    uint32_t overruns = 0;
    // Layout 2: the insert chain's latency, its IPC block included; 0 when
    // no insert on the track runs.
    uint32_t latency_frames = 0;
    std::array<InsertSlotSnapshot, kInsertSlotCount> inserts{};   // slot A first
};

// StateView::frame: the transport and every track from one publish.
struct EngineFrame {
    TransportSnapshot transport;
    std::vector<TrackSnapshot> tracks;   // info().track_count entries
};

// api::EventKind, spelled again for the same reason as TransportMode.
enum class EngineEventKind { Notice, InputsMissing, TakeFailed, Fatal, PluginFault };

// api::EngineEvent: a discrete fact, in the order the engine said it.
struct EngineEvent {
    EngineEventKind kind = EngineEventKind::Notice;
    uint64_t take = 0;    // TakeFailed: the take it is about
    uint64_t insert = 0;  // PluginFault: the insert's model id
    std::string text;     // InputsMissing, TakeFailed, PluginFault: empty clears it
};

// api::CommandReply.
struct CommandOutcome {
    uint64_t seq = 0;
    std::string command;  // carried back so the status line can name it
    DispatchResult result = DispatchResult::Ok;
    std::string message;  // the handler's reason when result is Failed
    // The answer to a live parameter (a fader ride, a plugin parameter), not
    // to a command: its Ok confirms one step of a gesture, so it must not
    // clear what the status line is saying -- the refusal at a ride's end
    // among others.
    bool live = false;
};

class EngineAdapter {
public:
    virtual ~EngineAdapter() = default;

    // Fails loudly with a reason the interface puts on screen.
    virtual bool connect(std::string& error) = 0;
    virtual const EngineInfo& info() const = 0;

    // Queues an Engine-domain command and returns its seq. Never blocks; the
    // result arrives through poll().
    virtual uint64_t send(const std::string& command, const CommandContext& ctx) = 0;
    virtual void poll(std::vector<CommandOutcome>& replies) = 0;
    // Appends the events poll() has received since the last call, oldest
    // first. They are held until taken, however many polls that is, so a
    // failed take or a fatal backend is never dropped between two reads.
    virtual void take_events(std::vector<EngineEvent>& out) = 0;

    // One consistent read of the state block. `out.tracks` is resized to
    // the track count; a caller that keeps its frame allocates once.
    virtual void frame(EngineFrame& out) const = 0;
    // Linear peak, the maximum since *this reader's* previous call, so no
    // peak between two ticks is lost and no other reader's is stolen.
    virtual float take_peak(uint32_t index) = 0;
    // False when the heartbeat has stalled. The interface then shows meters
    // as "no data" rather than frozen values (§3).
    virtual bool engine_alive(uint64_t now_ns) const = 0;

    // A fader being ridden: engine track `track`'s gain, linear 0 .. 4
    // (api::ParamId::Gain). Heard at once and not saved; the end of the
    // ride is saved with the model's SetMix (mix_model.h). Never blocks; a
    // refusal comes back through poll() as a command's does.
    virtual uint64_t send_track_gain(uint32_t track, float linear) = 0;

    // A plugin parameter moving (ADR-0012 §8): a drag in the generic
    // parameter panel, by insert id and stable key. Never blocks; a refusal
    // comes back through poll() as a command's does.
    virtual uint64_t send_plugin_parameter(uint64_t insert, const std::string& key,
                                           float value) = 0;
    // The latest PluginCatalog poll() has received since the last call, if
    // any; each replaces the last. api::PluginCatalog is used as it is, not
    // spelled again: it is plain data, and a field-for-field copy would be a
    // second catalog format to keep in step for nothing.
    virtual bool take_catalog(api::PluginCatalog& out) = 0;
};

}  // namespace sonora::ui
