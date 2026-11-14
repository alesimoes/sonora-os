// SPDX-License-Identifier: GPL-3.0-or-later
#include "bench_host.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "sonora/platform/process.h"
#include "sonora/plugin_chain_runner.h"
#include "sonora/plugin_client/host_supervisor.h"
#include "sonora/plugin_client/plugin_client.h"

namespace sonora::bench {

using namespace sonora::plugin_client;

namespace {

const char* const kLspLimiter = "http://lsp-plug.in/plugins/lv2/limiter_mono";
const char* const kDpl = "http://gareus.org/oss/lv2/dpl#mono";

// What the scan knows of a plugin: `--probe` prints its dense parameters.
// Copied from test_session_plugins.cpp.
struct Probe {
    bool ok = false;
    std::vector<std::string> symbols;
    std::vector<float> defaults;
};

Probe probe(const HostArgs& a, const std::string& id) {
    Probe p;
    const std::string out = (std::filesystem::temp_directory_path() /
                             ("sonora-graph-bench-probe-" + std::to_string(platform::current_process_id())))
                                .string();
    std::vector<std::string> argv = {a.host, "--probe", id};
    for (const std::string& b : a.bundles) {
        argv.push_back("--bundle");
        argv.push_back(b);
    }
    platform::ChildProcess::Options o;
    o.stdout_path = out;
    std::string error;
    auto child = platform::ChildProcess::spawn(argv, o, error);
    if (!child) return p;
    child->wait(60000);
    std::string all;
    {
        std::ifstream f(out);
        std::getline(f, all);
    }
    std::remove(out.c_str());
    p.ok = all.find("\"ok\":true") != std::string::npos;
    for (std::size_t at = all.find("{\"key\":\""); at != std::string::npos; at = all.find("{\"key\":\"", at + 1)) {
        const std::size_t s = all.find("\"symbol\":\"", at);
        const std::size_t d = all.find("\"default\":", at);
        if (s == std::string::npos || d == std::string::npos) break;
        p.symbols.push_back(all.substr(s + 10, all.find('"', s + 10) - (s + 10)));
        p.defaults.push_back(std::strtof(all.c_str() + d + 10, nullptr));
    }
    return p;
}

// A chain's spec from the scan: dense counts and defaults, then the
// overrides. False, with `why`, when a plugin is missing.
bool chain_spec(const HostArgs& a, const std::vector<HostPlugin>& plugins, ChainSpec& s, std::string& why) {
    s.channels = 1;
    for (const HostPlugin& pl : plugins) {
        const Probe p = probe(a, pl.id);
        if (!p.ok) {
            why = pl.id + " is not installed or refused its probe";
            return false;
        }
        std::vector<float> v = p.defaults;
        for (const auto& kv : pl.set) {
            const auto it = std::find(p.symbols.begin(), p.symbols.end(), kv.first);
            if (it == p.symbols.end()) {
                why = pl.id + " has no parameter " + kv.first;
                return false;
            }
            v[std::size_t(it - p.symbols.begin())] = kv.second;
        }
        s.plugins.push_back(pl.id);
        s.param_counts.push_back(uint32_t(v.size()));
        s.values.insert(s.values.end(), v.begin(), v.end());
    }
    s.bypass.assign(plugins.size(), 0.f);
    return true;
}

ClientConfig client_config(const BenchConfig& b) {
    ClientConfig c;
    c.block_frames = b.quantum;
    c.max_channels = b.tracks() + 8;
    c.max_stores = b.tracks() + 8;
    c.max_nodes = b.tracks() + 8;
    c.store_slots = 1024;
    return c;
}

SupervisorConfig sup_config(const BenchConfig& b, const HostArgs& a) {
    SupervisorConfig s;
    s.host_path = a.host;
    s.bundles = a.bundles;
    s.sample_rate = b.rate;
    s.block_frames = b.quantum;
    return s;
}

}  // namespace

std::vector<HostPlugin> default_aligned_chain() {
    const HostPlugin lsp{kLspLimiter, {{"lk", 20.f}}};
    const HostPlugin dpl{kDpl, {}};
    return {lsp, dpl, lsp, dpl};
}

struct HostChainSource::Impl {
    ClientConfig ccfg;
    PluginClient client;
    SupervisorConfig scfg;
    HostSupervisor sup;
    PluginChainRunner runner;
    std::vector<std::unique_ptr<SupervisedChain>> chains;  // by track; null: dry

    Impl(const BenchConfig& b, const HostArgs& a)
        : ccfg(client_config(b)), client(ccfg), scfg(sup_config(b, a)), sup(scfg, client), runner(client) {}
};

HostChainSource::HostChainSource(const BenchConfig& cfg, const HostArgs& args)
    : impl_(std::make_unique<Impl>(cfg, args)), cfg_(cfg) {
    if (!impl_->sup.ok()) {
        why_ = "the host does not speak this engine's versions: " + impl_->sup.why();
        return;
    }
    ChainSpec load, aligned;
    const std::vector<HostPlugin> aligned_chain =
        args.aligned_chain.empty() ? default_aligned_chain() : args.aligned_chain;
    if (!chain_spec(args, args.load_chain, load, why_) || !chain_spec(args, aligned_chain, aligned, why_)) {
        missing_ = true;
        return;
    }
    impl_->chains.resize(cfg.tracks());
    for (uint32_t t = 0; t < cfg.tracks(); ++t) {
        if (t == cfg.dry_track()) continue;
        // Store and node state by track: one chain per track.
        impl_->chains[t] = std::make_unique<SupervisedChain>(impl_->sup, t == cfg.aligned_track() ? aligned : load, t, t);
        std::string error;
        if (!impl_->chains[t]->start(error)) {
            why_ = "track " + std::to_string(t) + ": cannot start the chain: " + error;
            return;
        }
    }
    if (impl_->sup.region_lock_failures() != 0) {
        why_ = "a channel region could not be locked";
        return;
    }
    ok_ = true;
}

HostChainSource::~HostChainSource() = default;

InsertChainRunner& HostChainSource::runner() { return impl_->runner; }

uint32_t HostChainSource::latency(uint32_t track) const {
    const auto& c = impl_->chains;
    return track < c.size() && c[track] ? c[track]->latency_frames() : 0;
}

ChainBinding HostChainSource::binding(uint32_t track) const {
    ChainBinding b;
    const auto& c = impl_->chains;
    if (track >= c.size() || !c[track]) return b;
    const InsertChainRef r = c[track]->node();
    b.stable_key = track + 1;
    b.active = true;
    b.chain_id = 1000 + track;
    b.channel_slot = r.channel_slot;
    b.channel_generation = r.channel_generation;
    b.param_store = r.param_store;
    b.node_state = r.node_state;
    b.latency_frames = c[track]->latency_frames();
    return b;
}

uint64_t HostChainSource::host_misses(uint64_t callbacks) const {
    // The depth-1 protocol collects each block a callback later, so every
    // chain misses the first callback by design.
    const uint64_t chains = callbacks > 0 ? cfg_.tracks() - 1 : 0;
    const uint64_t missed = impl_->runner.missed();
    return missed > chains ? missed - chains : 0;
}

uint64_t HostChainSource::runner_drops() const {
    const PluginChainRunner& r = impl_->runner;
    return r.removed() + r.no_node_state() + r.unsupported();
}

}  // namespace sonora::bench
