// SPDX-License-Identifier: GPL-3.0-or-later
// The plugin catalog cache's path and the image build id, as the entry
// layer resolves them (ADR-0012, amendment of 2026-10-19, changes A, B).
#include "sonora/plugin_paths.h"

#include <cstdio>

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
    if (failures) {
        std::printf("test_plugin_paths: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_plugin_paths: ok\n");
    return 0;
}
