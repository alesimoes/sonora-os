// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/plugin_paths.h"

#include <sstream>

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

}  // namespace sonora
