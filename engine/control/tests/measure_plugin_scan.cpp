// SPDX-License-Identifier: GPL-3.0-or-later
//
// A real scan, measured by hand (ADR-0012, amendment of 2026-10-19;
// Unverified 11): the scan worker against a real sonora-plugin-host, cold
// (no cache) and then warm (the cache just written), with the probes each
// started and the wall time to a completed scan. Built with the tests so
// it cannot rot; never run by ctest (a cold scan of Debian's LSP bundles
// takes minutes, and its figure is the machine's).
//
//   measure_plugin_scan --host PATH [--cache PATH] [--slots N] [--bundle DIR]...
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#include "sonora/plugin_scanner.h"

using namespace sonora;

namespace {

uint64_t now_ms() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

void run(const char* what, const PluginScannerConfig& c) {
    const uint64_t t0 = now_ms();
    PluginScanner s(c);
    uint64_t ready_ms = 0;
    while (!(s.catalog()->scanned && s.idle())) {
        s.exchange(false);
        if (!ready_ms && s.catalog()->ready) ready_ms = now_ms() - t0;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    s.exchange(false);
    std::size_t usable = 0;
    for (const auto& p : s.catalog()->api.plugins) usable += p.usable ? 1 : 0;
    std::printf("%s: catalog published after %llu ms, scan complete after %llu ms; %llu probes, %llu lists, "
                "peak %u children; %zu plugins, %zu usable, generation %llu\n",
                what, (unsigned long long)ready_ms, (unsigned long long)(now_ms() - t0),
                (unsigned long long)s.probes_started(), (unsigned long long)s.lists_started(), s.peak_children(),
                s.catalog()->api.plugins.size(), usable, (unsigned long long)s.catalog()->generation);
    for (const std::string& n : s.take_notices()) std::printf("  notice: %s\n", n.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    PluginScannerConfig c;
    c.cache_path = (std::filesystem::temp_directory_path() / "sonora-measure-plugin-catalog.json").string();
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--host") c.host_path = argv[i + 1];
        else if (a == "--cache") c.cache_path = argv[i + 1];
        else if (a == "--slots") c.max_probes = uint32_t(std::atoi(argv[i + 1]));
        else if (a == "--bundle") c.bundles.push_back(argv[i + 1]);
    }
    if (c.host_path.empty()) {
        std::printf("usage: measure_plugin_scan --host PATH [--cache PATH] [--slots N] [--bundle DIR]...\n");
        return 2;
    }
    std::filesystem::remove(c.cache_path);
    run("cold", c);
    run("warm", c);
    std::filesystem::remove(c.cache_path);
    return 0;
}
