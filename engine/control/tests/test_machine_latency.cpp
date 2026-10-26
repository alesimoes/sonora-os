// SPDX-License-Identifier: GPL-3.0-or-later
//
// The machine's round trip kept between runs (ADR-0019, update of
// 2026-10-26): one line per capture device, rate and period; a save
// replaces its key's line and leaves the others; a file or a line that is
// not ours is ignored, never a guess.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "sonora/machine_latency.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

namespace fs = std::filesystem;
using namespace sonora;
using api::LatencySource;

fs::path fresh(const char* what) {
    const fs::path p = fs::temp_directory_path() / (std::string("sonora_test_machine_latency_") + what);
    std::error_code ec;
    fs::remove_all(p, ec);
    return p;
}

int keyed_per_device_rate_and_period() {
    const fs::path dir = fresh("keys");
    // The folder does not exist yet: the first save makes it.
    const std::string file = machine_latency_file((dir / "sonora").string());
    CHECK(file == (dir / "sonora" / "latency.conf").string());
    const LatencyKey umc{"UMC1820 Pro", 48000, 128};
    KeptLatency got;
    CHECK(!load_machine_latency(file, umc, got));
    std::string why;
    CHECK(save_machine_latency(file, umc, {287, LatencySource::Measured}, why));
    CHECK(load_machine_latency(file, umc, got));
    CHECK(got.frames == 287 && got.source == LatencySource::Measured);
    // Another period, rate or device is another round trip.
    CHECK(!load_machine_latency(file, {"UMC1820 Pro", 48000, 256}, got));
    CHECK(!load_machine_latency(file, {"UMC1820 Pro", 44100, 128}, got));
    CHECK(!load_machine_latency(file, {"Built-in Audio", 48000, 128}, got));
    const LatencyKey p256{"UMC1820 Pro", 48000, 256};
    CHECK(save_machine_latency(file, p256, {415, LatencySource::Manual}, why));
    // Replacing one key keeps the other.
    CHECK(save_machine_latency(file, umc, {290, LatencySource::Manual}, why));
    CHECK(load_machine_latency(file, umc, got) && got.frames == 290 &&
          got.source == LatencySource::Manual);
    CHECK(load_machine_latency(file, p256, got) && got.frames == 415);
    // No temporary file is left behind.
    int files = 0;
    for (const auto& e : fs::directory_iterator(dir / "sonora")) {
        (void)e;
        ++files;
    }
    CHECK(files == 1);
    // Only a measured or manual figure is kept; a device name that would
    // break the line is refused; no path, no file.
    CHECK(!save_machine_latency(file, umc, {1, LatencySource::Backend}, why));
    CHECK(!save_machine_latency(file, {"bad\nname", 48000, 128}, {1, LatencySource::Manual}, why));
    CHECK(!save_machine_latency("", umc, {1, LatencySource::Manual}, why));
    CHECK(machine_latency_file("").empty());
    CHECK(!load_machine_latency("", umc, got));
    fs::remove_all(dir);
    return 0;
}

int foreign_lines_ignored() {
    const fs::path dir = fresh("foreign");
    fs::create_directories(dir);
    const std::string file = (dir / "latency.conf").string();
    {
        std::ofstream out(file);
        out << "# a comment\n"
               "48000\t128\tlots\tmeasured\tUMC1820 Pro\n"
               "48000\t128\t300\tguessed\tUMC1820 Pro\n"
               "48000\t128\t300\n"
               "48000\t64\t200\tmeasured\tDevice With Spaces\n";
    }
    KeptLatency got;
    CHECK(!load_machine_latency(file, {"UMC1820 Pro", 48000, 128}, got));
    CHECK(load_machine_latency(file, {"Device With Spaces", 48000, 64}, got));
    CHECK(got.frames == 200 && got.source == LatencySource::Measured);
    fs::remove_all(dir);
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += keyed_per_device_rate_and_period();
    failed += foreign_lines_ignored();
    if (failed == 0) std::printf("machine_latency: all tests passed\n");
    return failed == 0 ? 0 : 1;
}
