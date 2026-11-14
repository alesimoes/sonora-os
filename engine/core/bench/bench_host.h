// SPDX-License-Identifier: GPL-3.0-or-later
//
// The real chains (ADR-0173 §1): every chain in its own sonora-plugin-host,
// started by the HostSupervisor, run by engine/core's PluginChainRunner, as
// test_session_plugins.cpp's Rig does. The load chains are the in-tree
// native plugins given on the command line (EQ, compressor, delay, reverb
// by default, mono builds); the aligned chain is test_session_plugins'
// aligned_2048 chain (LSP Limiter Mono at 20 ms twice, x42-dpl twice: 2048
// frames of their own), which sits below threshold at the bench's level and
// so passes the signal unchanged, bit for bit.
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "bench_session.h"

namespace sonora::bench {

struct HostPlugin {
    std::string id;
    std::vector<std::pair<std::string, float>> set;  // overrides by symbol
};

struct HostArgs {
    std::string host;
    std::vector<std::string> bundles;
    std::vector<HostPlugin> load_chain;
    std::vector<HostPlugin> aligned_chain;
};

// The aligned_2048 chain, for a HostArgs given none.
std::vector<HostPlugin> default_aligned_chain();

class HostChainSource final : public ChainSource {
public:
    HostChainSource(const BenchConfig& cfg, const HostArgs& args);
    ~HostChainSource() override;
    // Every host came up Ready; why() otherwise.
    bool ok() const { return ok_; }
    const std::string& why() const { return why_; }
    // A plugin of a chain is not installed: the run is skipped, not failed.
    bool missing() const { return missing_; }

    InsertChainRunner& runner() override;
    ChainBinding binding(uint32_t track) const override;
    uint32_t latency(uint32_t track) const override;
    uint64_t host_misses(uint64_t callbacks) const override;
    uint64_t runner_drops() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    BenchConfig cfg_;
    bool ok_ = false;
    bool missing_ = false;
    std::string why_;
};

}  // namespace sonora::bench
