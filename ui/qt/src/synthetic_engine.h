// SPDX-License-Identifier: GPL-3.0-or-later
//
// A stand-in engine behind the seam, so the shell has something to draw
// before M4. Deleted at M4 in favour of `FakeEngine`, which the rt-engineer
// owns (ADR-0007 §7: the consumer never keeps a private mock of a neighbour,
// because a private mock agrees with whatever its author already believes).
//
// What it is allowed to be:
//
//   - A *signal source*: deterministic per-channel levels, so meters,
//     peak hold and the over indicator can be seen and measured.
//   - A *command sink* for exactly the commands engine/control's
//     install_session_commands() installs today, with the same refusals
//     (no target, no such track, arming during a take). Copied by reading
//     that file, and the list is pinned in one place below so the drift is
//     visible. It does not record anything.
//
// What it must not become: the place where engine behaviour is designed.
// Anything the UI wants that the engine does not do yet is a request to the
// rt-engineer, not a branch in here. Tests of key routing use a recording
// adapter instead (tests/recording_adapter.h), so no test passes because this
// file agrees with itself.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "engine_adapter.h"

namespace sonora::ui {

class SyntheticEngine final : public EngineAdapter {
public:
    using Clock = std::function<uint64_t()>;  // monotonic nanoseconds

    // `tracks` defaults to the UMC1820's 18 inputs, as the engine does.
    explicit SyntheticEngine(uint32_t tracks = 18, Clock clock = {});

    // The engine stops answering at `ns` (monotonic clock): the heartbeat
    // stalls and every read after that is what a dead engine would leave.
    // For showing, and testing, the "no data" state.
    void die_at(uint64_t ns) { death_ns_ = ns; }

    bool connect(std::string& error) override;
    const EngineInfo& info() const override { return info_; }
    uint64_t send(const std::string& command, const CommandContext& ctx) override;
    void poll(std::vector<CommandOutcome>& replies) override;
    TransportSnapshot transport() const override;
    TrackSnapshot track(uint32_t index) const override;
    float take_peak(uint32_t index) override;
    bool engine_alive(uint64_t now_ns) const override;

    // The linear level channel `ch` shows at time `t_s`. Public so the tests
    // can check take_peak() returns the maximum over an interval, not a
    // sample at its end.
    static float level_at(uint32_t ch, double t_s);

    // The command ids install_session_commands() installs, less app.quit,
    // which under §3 is the interface's own and not the engine's.
    static const std::vector<std::string>& engine_commands();

private:
    bool handle(const std::string& command, const CommandContext& ctx,
                std::string& why);
    uint64_t now() const;

    EngineInfo info_;
    Clock clock_;
    uint64_t start_ns_ = 0;
    uint64_t death_ns_ = UINT64_MAX;
    bool connected_ = false;

    TransportMode mode_ = TransportMode::Stopped;
    uint64_t record_start_ns_ = 0;
    int32_t take_ = 1;
    std::vector<bool> armed_;
    std::vector<uint64_t> last_read_ns_;
    uint64_t seq_ = 0;
    std::deque<CommandOutcome> replies_;
};

}  // namespace sonora::ui
