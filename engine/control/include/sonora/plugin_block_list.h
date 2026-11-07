// SPDX-License-Identifier: GPL-3.0-or-later
//
// The operator's plug-in block list (ADR-0012, amendment of 2026-11-03):
// the plug-ins the operator has told the engine never to run. Unlike the
// catalog cache it is a setting, not regenerable, so it lives in the
// user's config folder and an unreadable file is said, never discarded in
// silence. The scan worker's thread is its only reader and writer.
//
//   {"version": 1, "blocked": [{"id": "<host id>", "name": "<as last scanned>"}]}
//
// The id is the host's: an LV2 URI, or "vst3:" and the class id. The name is
// for the operator reading the file, and for the catalog entry of a blocked
// plug-in the scan never probed.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace sonora {

inline constexpr uint32_t kPluginBlockListVersion = 1;
inline constexpr std::size_t kMaxPluginBlockListBytes = std::size_t{1} << 20;
inline constexpr std::size_t kMaxBlockedPlugins = 4096;
// A name is a label; a longer one in the file is cut on read.
inline constexpr std::size_t kMaxBlockedNameBytes = 256;

// What a blocked plug-in's catalog entry and chain start are refused with.
// Also the reason the scan caches for a blocked plug-in it did not probe,
// which is how such a record is told from a probe's own (Blocked) answer.
inline constexpr const char* kBlockedByOperator =
    "blocked by you: it is blocked in the plug-in manager";  // starts with api::kBlockedRefusalPrefix

struct PluginBlockList {
    std::map<std::string, std::string> plugins;  // host id -> name
    bool has(const std::string& host_id) const { return plugins.count(host_id) != 0; }
};

enum class BlockListLoad {
    Loaded,
    Missing,     // nothing blocked yet
    Unreadable,  // `why` says why; the file is left as it is
};

BlockListLoad load_plugin_block_list(const std::string& path, PluginBlockList& out, std::string& why);

std::string encode_plugin_block_list(const PluginBlockList& list);

// Written durably (write_file_durably). False, with `why`.
bool save_plugin_block_list(const std::string& path, const PluginBlockList& list, std::string& why);

}  // namespace sonora
