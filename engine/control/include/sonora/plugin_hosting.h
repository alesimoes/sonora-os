// SPDX-License-Identifier: GPL-3.0-or-later
//
// The engine's plugin side on the control thread (ADR-0012 §5.3, §8, and
// its amendment of 2026-10-14): one object owning engine/plugin_client's
// PluginClient, HostSupervisor and every SupervisedChain, and the
// PluginChainRunner the session calls, for one backend activation
// (amendment item 4). It lives on the document thread beside
// PlaybackController: the control server shows it every document, polls it
// every turn, and uses resolve() as the plan builder's ChainResolver.
// Nothing here runs on the audio thread, and nothing here waits: hosts are
// started and noticed from poll(). Probes are the scan worker's (the
// amendment of 2026-10-19, item 3, "one owner of probes"): a plugin the
// catalog cache already has costs no probe at all; a miss is asked of the
// worker, ahead of its scan, and its answer read from the worker's catalog.
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
// seeded from the document; a restart keeps its store (change F). What the
// document changed while that host started is posted once its store is in
// a plan. A session opened (start-up or ModelAction::Load, the same session
// reloaded included) starts every chain afresh: ids are per session.
//
// Parameter changes are tagged with a plan epoch (Session::post_plugin_param):
// the playing plan's, or the first plan built with the chain's store when
// that one is newer (a chain just swapped in). A change the audio thread
// drops (its plan replaced before it was drained) is re-posted from the
// document, bypass included, at most every 100 ms.
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
class PluginScanner;
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
    uint64_t ready_timeout_ms = 10000;
    std::string scratch_dir = "/tmp";
};

class PluginHosting {
public:
    // Builds the client for config.block_frames and two-channel chains,
    // the supervisor (the host's --version, the sealed clock) and the
    // runner, and installs the runner in the session. When the supervisor
    // refuses (a host of other versions, no seal), ok() is false and every
    // active chain plays silent, faulted (§5.5). `scanner` answers the
    // chains' probes and outlives this object (the control server owns
    // both); the caller runs its exchange() every turn.
    PluginHosting(Session& session, PluginHostingConfig config, PluginScanner& scanner);
    // Teardown order (item 4): the runner leaves the session; the caller
    // has deactivated the backend or seen a callback pass since; chains,
    // supervisor, client. Every host is asked to stop first and the whole
    // set gets one grace (2 s) before it is killed, not one per host.
    ~PluginHosting();
    PluginHosting(const PluginHosting&) = delete;
    PluginHosting& operator=(const PluginHosting&) = delete;

    bool ok() const { return ok_; }
    const std::string& why() const { return why_; }

    // The document changed. `opened`: it is a session just opened (start-up
    // or ModelAction::Load), whose chains all start afresh, from their
    // saved state, silent until Ready (item 1); no host of the document it
    // replaces is kept. Starts
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

    // Host processes alive, retiring and stopping ones included. For tests
    // and the log.
    uint32_t hosts() const;

    // What the running host of `insert` reads for `key` (empty: its bypass)
    // in its channel's last-value table: the store as the last submit
    // copied it. False when the insert has no running host or no such key.
    // For tests and diagnostics.
    bool host_value(model::Id insert, const std::string& key, float& value);

    // A host message of the chain on `track` (what poll() takes from each
    // chain): gestures post the value to the chain and, at their end, give
    // the document an op. Public so the mapping is testable without a
    // plugin editor.
    void handle_host_message(model::Id track, const std::string& bytes, const model::Snapshot& doc,
                             std::vector<model::Operation>& ops);

    struct Probe;
    struct Member;
    struct Chain;

private:
    // The probes this run has, by (plugin id, channels).
    Probe* probe_for(const model::PluginRef& plugin, uint16_t channels);
    // Asks the scan worker for the probes not yet asked.
    void start_probes();
    // True when an answer arrived in the worker's catalog.
    bool poll_probes();
    // The chain's spec as the document and the probes describe it now; false
    // while a probe runs.
    bool desired_spec(const model::TrackEdit& track, const model::Snapshot& doc, Chain& c);
    void start_pending(Chain& c, const model::Snapshot& doc, const std::vector<Member>& want);
    void retire(std::unique_ptr<plugin_client::SupervisedChain> chain, uint32_t store, uint32_t node);
    // A start no plan names, let go: stopped without waiting, reaped from
    // poll(); its store and node state are free at once.
    void abandon_pending(Chain& c);
    // A chain leaving (track gone, nothing left to run, a session opened).
    void drop_chain(Chain& c);
    void recompute_faults(const model::Snapshot& doc);
    // The running host's Loaded: instances whose saved state it refused.
    void note_refused_states(Chain& c, const model::Snapshot& doc);
    // The document's values the running chain's store does not have yet;
    // `force`: every one of them, bypass included (a dropped change).
    void post_values(Chain& c, const model::Snapshot& doc, bool force = false);
    // The epoch a change for the chain's store is tagged with; 0 while no
    // plan built names the store.
    uint64_t post_epoch(const Chain& c) const;
    // One change to the running store, or kept for a retry, newest wins.
    void post(Chain& c, uint32_t instance, uint32_t param, float value);
    // The running host's messages taken and mapped (poll does it every
    // turn). True when its latency moved.
    bool take_messages(Chain& c, const model::Snapshot& doc, std::vector<model::Operation>& ops);
    // Gestures a host left open (it died or was replaced mid-touch): the
    // last value touched goes to the document, as GestureEnd would have
    // (host_control.h, "A host that dies leaves gestures open").
    void close_gestures(Chain& c, const model::Snapshot& doc, std::vector<model::Operation>& ops);
    uint32_t take_store();
    uint32_t take_node();

    Session& session_;
    PluginHostingConfig config_;
    PluginScanner& scanner_;
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
    std::vector<std::unique_ptr<plugin_client::SupervisedChain>> stopping_;
    // Operations from show() (gestures a dropped chain left open), handed
    // out by the next poll().
    std::vector<model::Operation> deferred_ops_;
    bool reshow_ = false;
    std::vector<uint32_t> free_stores_;
    std::vector<uint32_t> free_nodes_;
    uint64_t last_built_epoch_ = 0;

    std::map<model::Id, std::string> faults_;
    uint32_t loading_total_ = 0;
    uint64_t dropped_seen_ = 0;
    uint64_t last_repost_ms_ = 0;
};

}  // namespace sonora
