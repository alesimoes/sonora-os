// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>

#include "sonora/interchange/aaf_export.h"
#include "sonora/model/model.h"

using namespace sonora::interchange;

static int fails = 0;
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
            ++fails;                                                \
        }                                                           \
    } while (0)

int main() {
    // The default exporter refuses loudly (no libaaf, or libaaf without write
    // support) and never leaves an empty file.
    auto ex = make_aaf_exporter();
    ConversionReport rep;
    std::string err;
    sonora::model::Snapshot empty = sonora::model::new_session({});
    CHECK(!ex->write(empty, "never.aaf", rep, err));
    CHECK(!err.empty());
    std::FILE* f = std::fopen("never.aaf", "rb");
    CHECK(f == nullptr);
    if (f) std::fclose(f);
    return fails ? 1 : 0;
}
