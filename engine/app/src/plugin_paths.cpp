// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/plugin_paths.h"

#include <filesystem>
#include <sstream>
#include <system_error>

namespace sonora {

std::string default_plugin_cache(const std::string& cache_dir) {
    return cache_dir.empty() ? std::string() : cache_dir + "/plugin-catalog.json";
}

std::string image_build_id(const std::string& os_release) {
    std::string build, image;
    std::istringstream in(os_release);
    for (std::string line; std::getline(in, line);) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
            value = value.substr(1, value.size() - 2);
        if (key == "BUILD_ID") build = value;
        else if (key == "IMAGE_VERSION") image = value;
    }
    return build.empty() ? image : build;
}

std::string default_wine_prefix(const char* wineprefix_env, const char* home_env) {
    if (wineprefix_env != nullptr && *wineprefix_env != '\0') return wineprefix_env;
    if (home_env != nullptr && *home_env != '\0') return std::string(home_env) + "/.wine";
    return {};
}

namespace {

[[maybe_unused]] std::string on_path(const char* path_env, const std::string& name) {
    if (path_env == nullptr) return {};
    std::istringstream dirs(path_env);
    for (std::string dir; std::getline(dirs, dir, ':');) {
        // An empty or relative entry names the working directory, which a
        // service has no business running Wine from.
        if (dir.empty() || dir.front() != '/') continue;
        std::error_code ec;
        const std::filesystem::path p = std::filesystem::path(dir) / name;
        const auto st = std::filesystem::status(p, ec);
        if (!ec && std::filesystem::is_regular_file(st) &&
            (st.permissions() & std::filesystem::perms::owner_exec) != std::filesystem::perms::none)
            return p.string();
    }
    return {};
}

}  // namespace

std::vector<std::string> wine_prefix_command(const char* path_env) {
#ifdef _WIN32
    (void)path_env;
    return {};  // yabridge is Linux's (ADR-0017); Windows hosts its own VST3s
#else
    const std::string wineboot = on_path(path_env, "wineboot");
    const std::string env = on_path(path_env, "env");
    if (wineboot.empty() || env.empty()) return {};
    return {env, "WINEDLLOVERRIDES=mscoree,mshtml=", wineboot, "--init"};
#endif
}

}  // namespace sonora
