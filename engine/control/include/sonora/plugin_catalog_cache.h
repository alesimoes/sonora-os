// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plugin catalog's cache on disk (ADR-0012, amendment of 2026-10-19,
// item 4 with changes A and B): where the scan worker keeps every probe
// record by bundle, with the bundle's fingerprint, so an engine start
// probes only what changed. Not for the audio thread or the control
// thread's turn: everything here touches the filesystem. The scan worker's
// thread (plugin_scanner.h) is its only caller in the engine.
//
//   {"version": 1, "host": "<--version line>", "build": "<image build id>",
//    "bundles": {"<path>": {"format": "lv2"|"vst3",
//                           "fingerprint": {"files", "bytes", "newest_ns", "complete"},
//                           "failed": "<why the bundle could not be scanned>",   (optional)
//                           "plugins": [<probe records, contracts/plugin probe_record.h>]}}}
//
// Written to a temporary file in the same directory, synced, renamed over
// the old one and the directory synced, so a power cut leaves the old cache
// or the new one, never half of one. Capped at 32 MiB. A file over the cap,
// unreadable, of another version, another host line or another build is
// discarded whole: the next scan is a full one.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "sonora/plugin/probe_record.h"

namespace sonora {

inline constexpr uint32_t kPluginCacheVersion = 1;
inline constexpr std::size_t kMaxPluginCacheBytes = std::size_t{32} << 20;
// Entries walked per bundle before the fingerprint gives up (complete =
// false): such a bundle is probed again at every scan.
inline constexpr uint32_t kMaxFingerprintEntries = 1024;

// Change B: the walk follows symlinks and stats their targets, and takes
// the newer of each entry's modification and change times, which no
// user-space copy can set back.
struct BundleFingerprint {
    uint64_t files = 0;     // regular files
    uint64_t bytes = 0;     // their total size
    int64_t newest_ns = 0;  // max(mtime, ctime) over every entry walked, the bundle's own included
    bool complete = true;   // false: over kMaxFingerprintEntries, or unreadable
    bool operator==(const BundleFingerprint& o) const {
        return files == o.files && bytes == o.bytes && newest_ns == o.newest_ns && complete == o.complete;
    }
    // Whether a cached entry with this fingerprint still describes the
    // bundle found now with `now`. Never true of an incomplete walk.
    bool matches(const BundleFingerprint& now) const { return complete && now.complete && *this == now; }
};

// A bundle directory walked recursively, or a single-file bundle (a VST3
// from before 3.6.10). A path that does not exist is incomplete, with no
// files.
BundleFingerprint fingerprint_bundle(const std::string& path);

struct CachedBundle {
    std::string format;  // "lv2" or "vst3"
    BundleFingerprint fingerprint;
    std::string failed;  // the whole bundle could not be enumerated: why
    std::vector<plugin::probe::Record> plugins;
};

struct PluginCatalogCache {
    std::string host;   // the --version line the records were written by
    std::string build;  // the image build id
    std::map<std::string, CachedBundle> bundles;
};

enum class CacheLoad {
    Loaded,
    Missing,    // no file: a first boot, or the cache was deleted
    Discarded,  // there was one, and `why` says why it is not used
};

// Reads `path` for `host` and `build`. A record inside that does not decode
// (another record version) drops that record alone, which is then probed
// again; anything wrong with the document itself discards it whole.
CacheLoad load_plugin_cache(const std::string& path, const std::string& host, const std::string& build,
                            PluginCatalogCache& out, std::string& why);

// The document as save_plugin_cache writes it. For tests.
std::string encode_plugin_cache(const PluginCatalogCache& cache);

// False, with `why`, when it could not be written durably or is over the
// cap (then nothing is written, and the old file stays).
bool save_plugin_cache(const std::string& path, const PluginCatalogCache& cache, std::string& why);

}  // namespace sonora
