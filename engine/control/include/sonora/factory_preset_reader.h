// SPDX-License-Identifier: GPL-3.0-or-later
//
// The engine's side of `sonora-plugin-host --read-preset` (ADR-0029 §5):
// one factory preset's content, read in a child of its own under the
// probe's timeout and crash rule, never in the engine. A preset file is
// untrusted input, and a VST3 program is read by running the plugin.
//
// Rules, each with a test (tests/test_factory_preset_reader.cpp):
//   - one child per read, at nice 10 and the idle I/O class, in a process
//     group of its own, dying with the engine (--engine-pid);
//   - a child that does not answer within timeout_ms is killed, and the
//     read is Blocked, with the reason;
//   - a child that crashes, or answers with something that is not a preset
//     read of what was asked (another plugin's state included), is Blocked
//     with the reason;
//   - a Blocked preset is remembered and answered Blocked again without a
//     child until forget_blocked() (a rescan, or the bundle changed): a
//     preset that crashes its reader is reported, never retried for ever;
//   - a child ended by our own stop (SIGINT, SIGTERM, SIGHUP, SIGQUIT)
//     says nothing about the preset: Blocked for this read, not remembered;
//   - Ok, Refused and NotInstalled are the host's answer, passed on as they
//     are, and not remembered.
//
// Blocking, so never on the audio thread or the control thread's turn: the
// scan worker's thread is its caller (ADR-0029 §5, slice 5 wires it). Not
// thread-safe: one caller.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "sonora/plugin/probe_record.h"

namespace sonora {

struct FactoryPresetReaderConfig {
    std::string host_path;             // sonora-plugin-host
    std::vector<std::string> bundles;  // --plugin-bundle, passed to every child
    std::string scratch_dir = "/tmp";
    uint64_t timeout_ms = 30000;  // PluginScannerConfig::probe_timeout_ms
    int nice = 10;
    bool idle_io = true;
};

class FactoryPresetReader {
public:
    explicit FactoryPresetReader(FactoryPresetReaderConfig config);

    // `bundle` is the plugin's (its probe record's), `plugin` the host id
    // ("vst3:" and the class id for a VST3), `preset` a preset id from the
    // plugin's record. Blocks until the child answers, or timeout_ms.
    plugin::probe::PresetRead read(const std::string& bundle, const std::string& plugin, const std::string& preset);

    // Everything Blocked may be tried again.
    void forget_blocked() { blocked_.clear(); }
    std::size_t blocked() const { return blocked_.size(); }
    uint64_t children_started() const { return children_; }

private:
    plugin::probe::PresetRead blocked(const std::string& plugin, const std::string& preset, std::string why,
                                      bool remember);

    const FactoryPresetReaderConfig config_;
    std::map<std::pair<std::string, std::string>, std::string> blocked_;  // (plugin, preset) -> why
    uint64_t children_ = 0;
};

}  // namespace sonora
