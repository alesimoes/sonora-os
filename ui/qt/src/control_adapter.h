// SPDX-License-Identifier: GPL-3.0-or-later
//
// EngineAdapter on the control API (ADR-0007 M4, M7): every call goes to an
// api::EngineControl and its StateView: ui/client's SocketEngine on the real
// engine's socket, or FakeEngine (contracts/api/testing) when no engine is
// running and in the tests. Either way this class and everything above it
// are unchanged.
//
// The Qt interface is not given InProcessEngine: ADR-0007 §3 keeps Qt, Mesa
// and LLVM out of the process that holds the take, so the Qt UI meets the
// real engine across the process boundary M7 builds.
//
// The model path shares the engine's inbox with the command path, so it is
// separated here: model updates, and the replies to requests made through
// submitModel(), go to the model sink (ReplicaDocument), never to the
// status line's command replies.
//
// No Qt type here, so it is tested without a display.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
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
    void take_events(std::vector<EngineEvent>& out) override;
    void frame(EngineFrame& out) const override;
    float take_peak(uint32_t index) override;
    float take_bus_peak(uint32_t side) override;
    bool engine_alive(uint64_t now_ns) const override;
    uint64_t send_track_param(uint32_t track, api::ParamId param, float value,
                              api::Gesture gesture) override;
    uint64_t send_send_level(uint64_t send, float linear) override;
    uint64_t send_aux_gain(uint64_t bus, float linear) override;
    uint64_t send_plugin_parameter(uint64_t insert, const std::string& key, float value) override;
    bool take_catalog(api::PluginCatalog& out) override;

    using ModelSink = std::function<void(const std::vector<api::ModelUpdate>& updates,
                                         const std::vector<api::CommandReply>& replies)>;
    // Called from poll() with what arrived for the model, possibly nothing.
    void setModelSink(ModelSink sink) { model_sink_ = std::move(sink); }
    uint64_t submitModel(const api::ModelRequest& request);

    // Which connection to the engine is up: 0 for none, and a new value for
    // each new one (SocketEngine's greetings). An engine in this process has
    // no connection to lose and needs no probe.
    using ConnectionProbe = std::function<uint64_t()>;
    // Told, after the model sink, when the connection went (false) or was
    // replaced (true). Every request without a reply by then is lost: its
    // seq is forgotten and a lost command is reported on the status line.
    using LinkSink = std::function<void(bool up)>;
    void watchConnection(ConnectionProbe probe, LinkSink sink);
    // link_error()'s source: SocketEngine::link_problem on the real engine.
    using LinkError = std::function<std::string()>;
    void setLinkError(LinkError probe) { link_error_ = std::move(probe); }
    std::string link_error() const override { return link_error_ ? link_error_() : std::string(); }

    // Requests unanswered are remembered up to this many each, the oldest
    // forgotten first, so an engine that never answers costs bounded memory.
    static constexpr std::size_t kMaxAwaiting = 1024;
    std::size_t awaiting_model_replies() const { return model_seqs_.size(); }
    std::size_t awaiting_command_replies() const { return pending_.size(); }

private:
    api::EngineControl& engine_;
    EngineInfo info_;
    ModelSink model_sink_;
    ConnectionProbe probe_;
    LinkSink link_sink_;
    LinkError link_error_;
    uint64_t connection_ = 0;
    std::set<uint64_t> model_seqs_;             // model requests awaiting a reply
    std::vector<api::CommandReply> model_replies_;
    struct Awaited {
        std::string name;   // the command, or what a live change moved, for the status line
        bool live = false;  // a live parameter (CommandOutcome::live)
    };
    std::map<uint64_t, Awaited> pending_;
    void await(uint64_t seq, std::string name, bool live);
    api::Inbox inbox_;
    std::vector<EngineEvent> events_;   // received, not yet taken
    api::PluginCatalog catalog_;        // the latest received
    bool catalog_new_ = false;          // and not yet taken
    mutable api::StateFrame state_;     // kept, so a tick does not allocate
};

}  // namespace sonora::ui
