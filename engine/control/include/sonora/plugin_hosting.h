// SPDX-License-Identifier: GPL-3.0-or-later
//
// The engine's plugin side on the control thread (ADR-0012 §5.3, §8, and
// its amendment of 2026-10-14): one object owning engine/plugin_client's
// PluginClient, HostSupervisor and every SupervisedChain, and the
// PluginChainRunner the session calls, for one backend activation
// (amendment item 4). It lives on the document thread beside
// PlaybackController: the control server shows it every document, polls it
// every turn, and uses resolve() as the plan builder's ChainResolver.
// Nothing here runs on the audio thread, and nothing here waits: probes and
// hosts are started and noticed from poll().
//
// One chain per track with an active insert (§2). What a chain plays while
// it has no Ready host follows the amendment's item 1: until its host is
// Ready, a chain plays what the playing plan already played for it.
//   running             its host's slot and checked latency
//   edited while running  the old host's binding until the new one is Ready;
//                       a start the document no longer describes (an undo
//                       before Ready) is abandoned and killed (change H)
//   new on a track      unbound, latency 0: dry and aligned, as before
//   restarting          the dead host's binding and latency (change A)
//   down, starting with the session, hosting disabled: kSilentChannelSlot,
//                       with the last checked latency (0 when none)
//   missing or refused instances are left out (changes B and C); a chain
//                       with none left is no chain, dry, its faults shown
//
// Retiring by epoch (item 2): a slot, a store and a node state named by a
// plan leave only once no live plan is older than the first plan built
// without them (PlaybackController::oldest_live_epoch).
//
// An edit is a new SupervisedChain with its own store and node state,
// seeded from the document; a restart keeps its store (change F).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "sonora/model/model.h"
#include "sonora/model/operations.h"
#include "sonora/plan_graph.h"

namespace sonora {

class Session;
class PluginChainRunner;
namespace plugin_client {
class PluginClient;
class HostSupervisor;
class SupervisedChain;
}  // namespace plugin_client
namespace platform {
class ChildProcess;
}

struct PluginHostingConfig {
    std::string host_path;             // sonora-plugin-host
    std::vector<std::string> bundles;  // --bundle, for probes and hosts
    std::string session_dir;           // where "Plugin State/" files are
    uint32_t sample_rate = 48000;
    uint32_t block_frames = 128;       // the backend's period (item 4)
    uint32_t max_block_frames = 1024;  // the session's PlaybackConfig::max_block_frames
    // Stores and node states: the session's PlaybackConfig::max_param_stores.
    uint32_t max_stores = 256;
    // Probes run at once (amendment item 3); 0: the hardware threads.
    uint32_t max_probes = 0;
    uint64_t probe_timeout_ms = 30000;
    uint64_t ready_timeout_ms = 10000;
    std::string scratch_dir = "/tmp";
};

class PluginHosting {
public:
    // Builds the client for config.block_frames and two-channel chains,
    // the supervisor (the host's --version, the sealed clock) and the
    // runner, and installs the runner in the session. When the supervisor
    // refuses (a host of other versions, no seal), ok() is false and every
    // active chain plays silent, faulted (§5.5).
    PluginHosting(Session& session, PluginHostingConfig config);
    // Teardown order (item 4): the runner leaves the session; the caller
    // has deactivated the backend or seen a callback pass since; chains,
    // supervisor, client.
    ~PluginHosting();
    PluginHosting(const PluginHosting&) = delete;
    PluginHosting& operator=(const PluginHosting&) = delete;

    bool ok() const { return ok_; }
    const std::string& why() const { return why_; }

    // The document changed. `opened`: it is a session just opened (start-up
    // or ModelAction::Load), whose chains start silent (item 1). Starts
    // probes and hosts for chains whose instances changed, abandons starts
    // the document no longer describes, and posts changed values (bypass,
    // params) to running chains. Never waits.
    void show(const model::Snapshot& doc, bool opened);

    struct PollResult {
        // A chain became Ready, went silent, or its latency moved: build a
        // plan from the document again.
        bool replan = false;
        // Values moved in a plugin's own editor (host gestures, §8): applied
        // to the document by the caller, which sends every replica the
        // update.
        std::vector<model::Operation> ops;
    };
    // Every turn of the control loop. `oldest_live_epoch`: the controller's,
    // for retiring by epoch.
    PollResult poll(uint64_t now_ms, const model::Snapshot& doc, uint64_t oldest_live_epoch);

    // PlaybackController's ChainResolver.
    void resolve(const model::TrackEdit& track, uint32_t engine_track, ChainBinding& binding);
    // A plan was built with `epoch`, after resolve() was asked for each
    // chain in it.
    void built(uint64_t epoch);

    // A client's PluginParameterChange (§8): resolved against the running
    // instance's own Loaded key table, refused outside the parameter's
    // range. False with `why` when it cannot be applied.
    bool set_param(model::Id insert, const std::string& key, float value, std::string& why);

    // Standing faults by insert id (item 5): the text names the plugin,
    // track and slot. An insert not here has none standing.
    const std::map<model::Id, std::string>& faults() const { return faults_; }
    // State-block flags for an insert (api InsertFlag bits Running,
    // Restarting, Faulted, Missing; the caller adds Present, Active and
    // Bypassed from the document).
    uint32_t insert_flags(model::Id insert) const;

    // Chains still starting with the session (for "Loading plug-ins (n of
    // m)"); 0 of 0 when none.
    uint32_t loading() const;
    uint32_t loading_total() const { return loading_total_; }

    // Host processes alive, retiring ones included. For tests and the log.
    uint32_t hosts() const;

    // A host message of the chain on `track` (what poll() takes from each
    // chain): gestures post the value to the chain and, at their end, give
    // the document an op. Public so the mapping is testable without a
    // plugin editor.
    void handle_host_message(model::Id track, const std::string& bytes, const model::Snapshot& doc,
                             std::vector<model::Operation>& ops);

    struct Probe;
    struct Chain;

private:
    // The probes this run has, by (plugin id, channels).
    Probe* probe_for(const model::PluginRef& plugin, uint16_t channels);
    void start_probes(uint64_t now_ms);
    // True when a probe finished.
    bool poll_probes(uint64_t now_ms);
    // The chain's spec as the document and the probes describe it now; false
    // while a probe runs.
    bool desired_spec(const model::TrackEdit& track, const model::Snapshot& doc, Chain& c);
    void start_pending(Chain& c, const model::Snapshot& doc);
    void retire(std::unique_ptr<plugin_client::SupervisedChain> chain, uint32_t store, uint32_t node);
    void recompute_faults(const model::Snapshot& doc);
    void post_values(Chain& c, const model::Snapshot& doc);
    uint32_t take_store();
    uint32_t take_node();

    Session& session_;
    PluginHostingConfig config_;
    bool ok_ = false;
    std::string why_;
    std::unique_ptr<plugin_client::PluginClient> client_;
    std::unique_ptr<plugin_client::HostSupervisor> supervisor_;
    std::unique_ptr<PluginChainRunner> runner_;

    std::map<std::pair<std::string, uint16_t>, std::unique_ptr<Probe>> probes_;
    std::map<model::Id, std::unique_ptr<Chain>> chains_;   // by track id

    struct Retiring {
        std::unique_ptr<plugin_client::SupervisedChain> chain;
        uint32_t store;
        uint32_t node;
        uint64_t first_epoch_without = 0;  // 0: not yet built without it
    };
    std::vector<Retiring> retiring_;
    std::vector<uint32_t> free_stores_;
    std::vector<uint32_t> free_nodes_;
    uint64_t last_built_epoch_ = 0;

    std::map<model::Id, std::string> faults_;
    uint32_t loading_total_ = 0;
    uint64_t dropped_seen_ = 0;
    uint64_t last_repost_ms_ = 0;
};

}  // namespace sonora
