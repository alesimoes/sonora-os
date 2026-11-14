// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>

#include "fake_aaf_writer.h"
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
    ConversionReport rep;
    std::string err;
    sonora::model::Snapshot empty = sonora::model::new_session({});

    AafExporter none(nullptr);
    CHECK(!none.write(empty, "a.aaf", rep, err));
    CHECK(err.find("no AAF writer") != std::string::npos);

    FakeAafWriter fake;
    AafExporter ex(&fake);
    err.clear();
    CHECK(!ex.write(empty, "a.aaf", rep, err));
    CHECK(err.find("no tracks") != std::string::npos);
    CHECK(fake.calls == 0);
    return fails ? 1 : 0;
}
