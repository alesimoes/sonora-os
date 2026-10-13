// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// LV2 hosting through lilv (ADR-0007 §1: nothing outside plugins/host links
// a plugin SDK). Lv2World finds plugins and makes Lv2Processors for a Chain.

#include "processor.h"
#include "urid_map.h"

#include <lilv/lilv.h>

#include <memory>
#include <string>
#include <vector>

namespace sonora::plugin_host {

class Lv2World : public PluginSource {
public:
    Lv2World();
    ~Lv2World() override;
    Lv2World(const Lv2World&) = delete;
    Lv2World& operator=(const Lv2World&) = delete;

    // Installed plugins come from LV2_PATH or lilv's default path. A test
    // bundle is added explicitly so the test does not depend on the user's.
    bool load_bundle(const std::string& dir, std::string& error);

    const LilvPlugin* find(const std::string& uri) const;
    std::vector<const LilvPlugin*> all() const;
    PluginInfo describe(const LilvPlugin* plugin) const;

    std::unique_ptr<Processor> instantiate(const std::string& uri, const ChainConfig& config,
                                           const std::vector<float*>& in,
                                           const std::vector<float*>& out,
                                           const std::vector<float*>& key,
                                           ChainError& error) override;

    LilvWorld* world() const { return world_; }
    UridMap& urids() { return urids_; }
    static bool feature_supported(const std::string& uri);

    struct Nodes {
        LilvNode *audio, *control, *atom, *cv, *input, *output;
        LilvNode *optional, *reports_latency, *latency_designation, *enabled_designation, *sidechain;
    } n{};

private:
    LilvWorld* world_ = nullptr;
    UridMap urids_;
};

}  // namespace sonora::plugin_host
