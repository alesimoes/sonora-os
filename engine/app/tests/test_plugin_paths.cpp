// SPDX-License-Identifier: GPL-3.0-or-later
// The plugin catalog cache's path and the image build id, as the entry
// layer resolves them (ADR-0012, amendment of 2026-10-19, changes A, B).
#include "sonora/plugin_paths.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace sonora;

namespace {
int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)
}  // namespace

int main() {
    CHECK(default_plugin_cache("/home/op/.cache/sonora") == "/home/op/.cache/sonora/plugin-catalog.json");
    CHECK(default_plugin_cache("").empty());

    CHECK(image_build_id("PRETTY_NAME=\"Debian GNU/Linux 13 (trixie)\"\nBUILD_ID=\"20261019.1\"\n") == "20261019.1");
    CHECK(image_build_id("IMAGE_VERSION=7\nID=sonora\n") == "7");
    // BUILD_ID wins over IMAGE_VERSION, wherever it comes.
    CHECK(image_build_id("BUILD_ID=a\nIMAGE_VERSION=b\n") == "a");
    CHECK(image_build_id("IMAGE_VERSION=b\nBUILD_ID='a'\r\n") == "a");
    CHECK(image_build_id("ID=debian\nVERSION_ID=\"13\"\n").empty());
    CHECK(image_build_id("").empty());

    // ADR-0017 Y4: the prefix as Wine picks it.
    CHECK(default_wine_prefix("/srv/wine", "/home/op") == "/srv/wine");
    CHECK(default_wine_prefix("", "/home/op") == "/home/op/.wine");
    CHECK(default_wine_prefix(nullptr, "/home/op") == "/home/op/.wine");
    CHECK(default_wine_prefix(nullptr, nullptr).empty());
#ifndef _WIN32
    {
        namespace fs = std::filesystem;
        const fs::path root = fs::absolute("test_plugin_paths_wine");
        fs::remove_all(root);
        fs::create_directories(root / "bin");
        fs::create_directories(root / "usr");
        const auto exe = [](const fs::path& p) {
            std::ofstream(p) << "#!/bin/sh\n";
            fs::permissions(p, fs::perms::owner_all);
        };
        exe(root / "bin" / "env");
        // Not executable: skipped, as a shell would.
        std::ofstream(root / "bin" / "wineboot") << "";
        fs::permissions(root / "bin" / "wineboot", fs::perms::owner_read | fs::perms::owner_write);
        const std::string path = "relative/dir::" + (root / "bin").string() + ":" + (root / "usr").string();
        CHECK(wine_prefix_command(path.c_str()).empty());  // no wineboot: Wine not installed
        exe(root / "usr" / "wineboot");
        const std::vector<std::string> cmd = wine_prefix_command(path.c_str());
        CHECK(cmd.size() == 4 && cmd[0] == (root / "bin" / "env").string() &&
              cmd[1] == "WINEDLLOVERRIDES=mscoree,mshtml=" && cmd[2] == (root / "usr" / "wineboot").string() &&
              cmd[3] == "--init");
        CHECK(wine_prefix_command(nullptr).empty());
        fs::remove_all(root);
    }
#endif
    if (failures) {
        std::printf("test_plugin_paths: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_plugin_paths: ok\n");
    return 0;
}
