// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// VST3 hosting against Steinberg's interface headers (pluginterfaces only:
// no SDK hosting helpers, so every object the plugin calls back into is ours
// and allocation-free on the processing thread). ADR-0007 §1: nothing
// outside plugins/host includes a plugin SDK.
//
// A class is named "vst3:" followed by its 16-byte class ID in hex, in byte
// order, which is the "CID" a bundle's moduleinfo.json carries on Linux:
//   vst3:84E8DE5F92554F5396FAE4133C935A18   (Steinberg's AGain)
//
// Parameters (contracts/plugin): ParamChange::param is the DENSE index of a
// writable parameter in controller order, mapped here to its ParamID, and
// ParamChange::value the NORMALISED value, 0..1, clamped, delivered at its
// frame offset. --list gives each parameter's title, units, normalised
// default and dense index; --probe its key (the ParamID in decimal).

#include "processor.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sonora::plugin_host {

struct Vst3Module;
class Vst3HostContext;

class Vst3World : public PluginSource {
public:
    Vst3World();
    ~Vst3World() override;
    Vst3World(const Vst3World&) = delete;
    Vst3World& operator=(const Vst3World&) = delete;

    static bool is_vst3_id(const std::string& id);

    // A bundle given on the command line, searched before the default roots
    // (loader.h, vst3_default_roots).
    void add_bundle(const std::string& path);
    // Every bundle: the explicit ones, then each *.vst3 under the roots.
    std::vector<std::string> bundles() const;

    // Load one bundle in this process and describe each of its effect
    // classes as instantiated at 48 kHz / 128, latency included, handing
    // each to `each` as soon as it is known, so a class that hangs or crashes
    // later does not take the earlier ones with it. Runs plugin code: --list
    // calls it through run_isolated, one bundle per child.
    bool describe_bundle(const std::string& bundle, const std::function<void(const PluginInfo&)>& each,
                         std::string& error);
    // One class by id: what --probe needs to pick a channel count.
    bool describe(const std::string& id, PluginInfo& out, ChainError& error);

    std::unique_ptr<Processor> instantiate(const std::string& id, const ChainConfig& config,
                                           const std::vector<float*>& in,
                                           const std::vector<float*>& out,
                                           const std::vector<float*>& key,
                                           ChainError& error) override;

private:
    Vst3Module* load(const std::string& bundle, std::string& error);
    Vst3Module* module_for(const std::string& id, int& class_index, ChainError& error);

    std::vector<std::string> explicit_;
    std::vector<std::unique_ptr<Vst3Module>> modules_;
    std::unique_ptr<Vst3HostContext> context_;
};

}  // namespace sonora::plugin_host
