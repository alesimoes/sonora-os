// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where the plugin catalog cache lives and which image wrote it (ADR-0012,
// amendment of 2026-10-19, item 4 with changes A and B, and the owner's
// Q2). The entry layer resolves both and passes them down as plain values:
// engine/control reads no environment and knows no ~/.cache.
#pragma once

#include <string>

namespace sonora {

// `cache_dir` is common/platform's user_cache_dir() (~/.cache/sonora on
// the appliance, whose /home is the persistence partition's union mount,
// ADR-0005). Empty when there is none: the catalog lives in memory.
std::string default_plugin_cache(const std::string& cache_dir);

// The image's build id from an os-release file's text: BUILD_ID, else
// IMAGE_VERSION (os-release(5)), unquoted; empty when it names neither (a
// developer's machine). An image update changes it, so the cache's system
// bundles are fingerprinted afresh even where reproducible builds clamp
// their mtimes.
std::string image_build_id(const std::string& os_release);

}  // namespace sonora
