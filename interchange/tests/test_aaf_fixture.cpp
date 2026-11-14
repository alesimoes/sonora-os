// SPDX-License-Identifier: GPL-3.0-or-later
// Registered only with SONORA_WITH_LIBAAF. Imports tests/fixtures/aaf/minimal.aaf
// (CC0, authored for this project) through libaaf and the mapper. Exit 77 is
// ctest's SKIP code: a missing fixture shows as "skipped", never as a pass.
#include <cstdio>
#include <filesystem>
#include <memory>

#include "aaf_map.h"
#include "sonora/interchange/aaf_import.h"

using namespace sonora::interchange;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

int main() {
    const std::filesystem::path fx =
        std::filesystem::path(SONORA_AAF_FIXTURE_DIR) / "minimal.aaf";
    if (!std::filesystem::exists(fx)) {
        std::printf("SKIP: %s is not committed (libaaf was needed to author it)\n",
                    fx.string().c_str());
        return 77;
    }
    std::unique_ptr<AafReader> reader(make_libaaf_reader());
    CHECK(reader != nullptr);
    if (!reader) return 1;

    AafDocument doc;
    std::string error;
    CHECK(reader->read(fx.string(), doc, error));
    CHECK(!doc.mobs.empty());

    sonora::model::Snapshot snap;
    ConversionReport report;
    error.clear();
    CHECK(map_aaf(doc, 48000, snap, report, error));
    CHECK(error.empty());
    return failures ? 1 : 0;
}
