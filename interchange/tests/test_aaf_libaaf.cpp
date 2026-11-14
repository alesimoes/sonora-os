// SPDX-License-Identifier: GPL-3.0-or-later
// Registered only with SONORA_WITH_LIBAAF. The fixture, when present, is
// tests/fixtures/minimal.aaf (generated for this project, CC0).
#include <cstdio>
#include <memory>
#include <filesystem>

#include "sonora/interchange/aaf_import.h"

using namespace sonora::interchange;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

int main() {
    std::unique_ptr<AafReader> reader(make_libaaf_reader());
    CHECK(reader != nullptr);
    if (!reader) return 1;

    AafDocument doc;
    std::string error;
    CHECK(!reader->read("no-such-file.aaf", doc, error));
    CHECK(!error.empty());

    const std::filesystem::path fx = std::filesystem::path(SONORA_AAF_FIXTURES) / "minimal.aaf";
    if (!std::filesystem::exists(fx)) {
        std::printf("fixture minimal.aaf absent: only the error path was checked\n");
        return failures ? 1 : 0;
    }
    AafDocument d;
    error.clear();
    CHECK(reader->read(fx.string(), d, error));
    CHECK(!d.mobs.empty());
    return failures ? 1 : 0;
}
