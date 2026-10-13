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
//   EngineAdapter::transport    StateView::transport  (TransportSnapshot)
//   EngineAdapter::track        StateView::track      (TrackSnapshot)
//   EngineAdapter::take_peak    StateView::take_peak  (per-reader cursor)
//   EngineAdapter::engine_alive StateView::engine_alive (heartbeat)
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

#include <cstdint>
#include <string>
#include <vector>

#include "sonora/command.h"

namespace sonora::ui {

// api::Hello, less the version fields: there is no wire here to mismatch.
struct EngineInfo {
    uint32_t sample_rate = 0;
    uint32_t buffer_frames = 0;
    uint32_t track_count = 0;
    // What the engine has handlers for. Feeds KeyMap::ensure_essentials()
    // together with the interface's own registry, as §5 says.
    std::vector<std::string> implemented_commands;
};

// The engine's TransportState, spelled again because ui/ may not include
// engine/ (ADR-0007 §1). §4.1's TransportFields carries the same three.
enum class TransportMode { Stopped, Rolling, Recording };

// Health flags from §4.1's EngineStateBlock::health_flags.
enum HealthFlag : uint32_t {
    kHealthServerGone = 1u << 0,
    kHealthNoInputs = 1u << 1,
};

struct TransportSnapshot {
    TransportMode state = TransportMode::Stopped;
    uint64_t playhead_frames = 0;
    int32_t take = 0;          // the take that is open, or the next one
    uint32_t xruns = 0;
    uint32_t health_flags = 0;
    // Update 2026-10-12 item 3: a seqlock read that ran out of retries. The
    // interface shows it as stale for that tick rather than spinning.
    bool stale = false;
};

struct TrackSnapshot {
    bool armed = false;
    bool muted = false;
    uint32_t overruns = 0;
};

// api::CommandReply.
struct CommandOutcome {
    uint64_t seq = 0;
    std::string command;  // carried back so the status line can name it
    DispatchResult result = DispatchResult::Ok;
    std::string message;  // the handler's reason when result is Failed
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

    virtual TransportSnapshot transport() const = 0;
    virtual TrackSnapshot track(uint32_t index) const = 0;
    // Linear peak, the maximum since *this reader's* previous call, so no
    // peak between two ticks is lost and no other reader's is stolen.
    virtual float take_peak(uint32_t index) = 0;
    // False when the heartbeat has stalled. The interface then shows meters
    // as "no data" rather than frozen values (§3).
    virtual bool engine_alive(uint64_t now_ns) const = 0;
};

}  // namespace sonora::ui
