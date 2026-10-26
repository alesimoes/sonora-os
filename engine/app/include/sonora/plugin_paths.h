// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where the plugin catalog cache lives and which image wrote it (ADR-0012,
// amendment of 2026-10-19, item 4 with changes A and B, and the owner's
// Q2). The entry layer resolves both and passes them down as plain values:
// engine/control reads no environment and knows no ~/.cache.
#pragma once

#include <string>
#include <vector>

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

// The Wine prefix Windows plug-ins run in under yabridge (ADR-0017 Y4), as
// Wine itself picks it: $WINEPREFIX, else ~/.wine. Empty when neither is
// known. Plain values in, so the rule is testable.
std::string default_wine_prefix(const char* wineprefix_env, const char* home_env);

// The command that creates that prefix, for the scanner to run before it
// scans anything bridged: `wineboot --init` by absolute path, found on
// `path_env` (children are spawned without a PATH lookup), under `env`
// with Wine's Mono and Gecko installers turned off: on a kiosk nobody is
// there to answer their dialogs, which would hold the prefix past its
// timeout. Empty when wineboot is not on the path (Wine not installed).
std::vector<std::string> wine_prefix_command(const char* path_env);

// `wineserver -w`, which returns once the prefix's registry has been saved
// (wineboot exits before that); empty when wineserver is not on PATH.
std::vector<std::string> wine_server_wait_command(const char* path_env);

}  // namespace sonora
