// SPDX-License-Identifier: GPL-3.0-or-later
//
// EngineAdapter on the control API (ADR-0007 M4): every call goes to an
// api::EngineControl and its StateView. Behind it today is FakeEngine
// (contracts/api/testing), the provider's fake; after M7 it is the socket
// client to the real engine. Either way this class and everything above it
// are unchanged.
//
// The Qt interface is not given InProcessEngine: ADR-0007 §3 keeps Qt, Mesa
// and LLVM out of the process that holds the take, so the Qt UI meets the
// real engine across the process boundary M7 builds, not before.
//
// No Qt type here, so it is tested without a display.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "engine_adapter.h"
#include "sonora/api/engine_control.h"

namespace sonora::ui {

class ControlAdapter final : public EngineAdapter {
public:
    explicit ControlAdapter(api::EngineControl& engine) : engine_(engine) {}

    bool connect(std::string& error) override;
    const EngineInfo& info() const override { return info_; }
    uint64_t send(const std::string& command, const CommandContext& ctx) override;
    void poll(std::vector<CommandOutcome>& replies) override;
    TransportSnapshot transport() const override;
    TrackSnapshot track(uint32_t index) const override;
    float take_peak(uint32_t index) override;
    bool engine_alive(uint64_t now_ns) const override;

    // Events since the last poll(): notices, missing inputs, a failed take,
    // a fatal backend. The shell does not draw them yet; they are kept for
    // one poll so a caller that wants them can read them.
    const std::vector<api::EngineEvent>& events() const { return inbox_.events; }

private:
    api::EngineControl& engine_;
    EngineInfo info_;
    std::map<uint64_t, std::string> pending_;   // seq -> command, for the status line
    api::Inbox inbox_;
};

}  // namespace sonora::ui
