// SPDX-License-Identifier: GPL-3.0-or-later
//
// The control thread's side of the plugin boundary (ADR-0012 §4, §5.3):
// starts sonora-plugin-host processes, brings each to Ready, binds it to a
// channel slot of PluginClient, watches it, and restarts it make-before-
// break. Never on the audio thread: everything here allocates, starts
// processes or waits.
//
// A STUB of the supervisor ADR-0012 checklist 4 describes, on what the host
// has today:
//   - the host is told its chain on the command line (--chain, --bundle,
//     --set) and finds its regions at descriptors 3 and 4 (--shm-fd 3
//     --clock-fd 4), because contracts/plugin/host_control.h is not written.
//     TODO(host_control.h, BACKLOG Track 4 item 4): the fd-3 SOCK_SEQPACKET
//     channel (Hello with the wire version, Load, Loaded/Refused, GetState
//     for make-before-break state, Shutdown), the regions crossing it with
//     SCM_RIGHTS, and `wire N` in the --version line.
//   - the host is watched by polling (ChildProcess, waitpid WNOHANG) from
//     poll(); TODO(pidfd): a pidfd in the control server's poll set.
//   - the state of the instances that carry over a restart or an edit is
//     not fetched (no GetState without host_control.h): a restarted host
//     starts from its seeded parameters.
// Hosts must be started from the control server's long-lived thread: the
// host's death signal follows the thread that started it (plugins/host
// ipc.h, die_with_parent).
#pragma once

#include "sonora/plugin/protocol.h"
#include "sonora/plugin_client/plugin_client.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace sonora::platform {
class ChildProcess;
class SharedRegion;
}  // namespace sonora::platform

namespace sonora::plugin_client {

struct SupervisorConfig {
    std::string host_path;             // sonora-plugin-host
    std::vector<std::string> bundles;  // --bundle, for every host
    uint32_t sample_rate = 48000;
    uint32_t block_frames = 128;
    uint32_t max_frames = 1024;
    int32_t host_rt_priority = 70;  // below the audio thread's 80 (ADR-0012 §2)
    uint64_t ready_timeout_ms = 10000;    // §5.3
    uint64_t heartbeat_timeout_ms = 500;  // §4: a hang outside a plugin
    uint32_t max_restarts = 3;            // §5.3: at most 3 in 60 s
    uint64_t restart_window_ms = 60000;
    // Where `--version` output is captured at start-up.
    std::string scratch_dir = "/tmp";
};

// One chain as the scan and the model describe it.
struct ChainSpec {
    std::vector<std::string> plugins;    // LV2 URIs or vst3:<class id>, in slot order
    std::vector<uint32_t> param_counts;  // dense parameters of each, from the scan
    uint32_t channels = 1;               // signal channels, plus key_channels after them
    uint32_t key_channels = 0;           // host --sidechain
    // The model's values, seeded into the table before the host starts so it
    // instantiates with them (§5.1): param_slots values, then one bypass per
    // instance. Empty: zeros.
    std::vector<float> values;
    std::vector<float> bypass;
    // Extra host arguments until host_control.h's Load carries them
    // (--set INDEX:SYMBOL=VALUE, --reactivate).
    std::vector<std::string> extra_args;
};

// One running host and its channel region. Owned by a SupervisedChain.
class HostProcess {
public:
    ~HostProcess();
    HostProcess(const HostProcess&) = delete;
    HostProcess& operator=(const HostProcess&) = delete;

    plugin::ChannelHeader& header() { return *header_; }
    void* region() { return base_; }
    const plugin::Layout& layout() const { return layout_; }
    const plugin::ParamMap& map() const { return map_; }
    // chain + IPC block, through checked_latency, read after Ready. What the
    // plan builder puts in the InsertChain node for PDC (§7.3).
    uint32_t latency_frames() const { return latency_; }

    bool running();
    // Ready and its heartbeat has not moved for heartbeat_timeout_ms.
    bool hung(uint64_t now_ms);
    // shutdown word, a bounded wait, then SIGKILL; reaps it.
    void stop();
    void kill();
    // "exited with code N" / "killed by signal N", once it is not running.
    std::string exit_text();

private:
    friend class HostSupervisor;
    HostProcess() = default;

    std::unique_ptr<platform::SharedRegion> region_;
    std::unique_ptr<platform::ChildProcess> child_;
    void* base_ = nullptr;
    plugin::ChannelHeader* header_ = nullptr;
    plugin::Layout layout_;
    plugin::ParamMap map_;
    uint32_t latency_ = 0;
    uint64_t last_beat_ = 0;
    uint64_t last_beat_ms_ = 0;
    uint64_t heartbeat_timeout_ms_ = 0;
};

class HostSupervisor {
public:
    // Creates the session's clock region (sealed, read-only for hosts) and
    // runs `host --version`. ok() is false, with why() saying so, when the
    // seal is not available (auditor L2) or the host does not speak this
    // engine's layout and clock versions (§5.5): plugin hosting is then
    // disabled for the run, loudly, and recording is untouched.
    HostSupervisor(const SupervisorConfig& config, PluginClient& client);
    ~HostSupervisor();
    HostSupervisor(const HostSupervisor&) = delete;
    HostSupervisor& operator=(const HostSupervisor&) = delete;

    bool ok() const { return ok_; }
    const std::string& why() const { return why_; }
    const SupervisorConfig& config() const { return config_; }
    PluginClient& client() { return client_; }
    // Region pages the OS would not lock (auditor L3); reported, not fatal.
    uint64_t region_lock_failures() const { return lock_failures_; }

    // A host for `spec`, brought to Ready within ready_timeout_ms. Null with
    // `error` naming the plugin and the reason (refusal, exit code) when it
    // does not get there.
    std::unique_ptr<HostProcess> start(const ChainSpec& spec, std::string& error);

private:
    SupervisorConfig config_;
    PluginClient& client_;
    std::unique_ptr<platform::SharedRegion> clock_region_;
    bool ok_ = false;
    std::string why_;
    uint64_t lock_failures_ = 0;
};

// One insert chain under supervision: its host, its channel slot, and the
// restart policy of §5.3. Control thread only.
class SupervisedChain {
public:
    enum class Event {
        None,
        Swapped,   // a new host is bound: put node() in a new plan, then call retired()
        Down,      // restarts exhausted (3 in 60 s): the path stays silent; why() says so
    };

    SupervisedChain(HostSupervisor& supervisor, ChainSpec spec, uint32_t param_store, uint32_t node_state);
    ~SupervisedChain();
    SupervisedChain(const SupervisedChain&) = delete;
    SupervisedChain& operator=(const SupervisedChain&) = delete;

    // The first host. Resets the store from spec.values and the node state.
    bool start(std::string& error);

    // What the plan's InsertChain node carries now (§7.1).
    InsertChainRef node() const;
    uint32_t latency_frames() const;
    HostProcess* host() { return current_.get(); }

    // Every 50 ms or so. A host that exited, hung, or whose slot the audio
    // thread removed after 8 misses is killed, and if the policy allows a
    // new one is started while the old slot stays in the plan
    // (make-before-break); Swapped asks the caller to publish a plan with
    // node(). `now_ms` is a steady clock.
    Event poll(uint64_t now_ms);
    // The plan that named the previous slot has been retired (ADR-0009
    // §4.1): its host is stopped, its region released, its slot freed.
    void retired();

    uint32_t restarts() const { return restarts_total_; }
    bool down() const { return down_; }
    const std::string& why() const { return why_; }

private:
    bool replace(uint64_t now_ms, const std::string& reason);
    bool bind(std::unique_ptr<HostProcess> host);

    HostSupervisor& sup_;
    ChainSpec spec_;
    uint32_t store_;
    uint32_t node_;
    std::unique_ptr<HostProcess> current_;
    uint32_t slot_ = kNoSlot;
    uint32_t generation_ = 0;
    std::unique_ptr<HostProcess> retiring_;
    uint32_t retiring_slot_ = kNoSlot;
    std::deque<uint64_t> restart_times_;
    uint32_t restarts_total_ = 0;
    bool down_ = false;
    std::string why_;
};

}  // namespace sonora::plugin_client
