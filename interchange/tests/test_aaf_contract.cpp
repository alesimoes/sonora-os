// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>

#include "fake_aaf_reader.h"
#include "sonora/interchange/aaf_import.h"

using namespace sonora::interchange;

static int fails = 0;
#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);    \
            ++fails;                                                   \
        }                                                              \
    } while (0)

int main() {
    FakeAafReader fake;  // empty document
    AafImporter imp(&fake);
    CHECK(imp.can_read("a.aaf"));
    sonora::model::Snapshot snap;
    ConversionReport rep;
    std::string err;
    CHECK(!imp.read("a.aaf", snap, rep, err));
    CHECK(err.find("no mobs") != std::string::npos);

    AafImporter none(nullptr);
    CHECK(!none.can_read("a.aaf"));
    err.clear();
    CHECK(!none.read("a.aaf", snap, rep, err));
    CHECK(!err.empty());
    return fails ? 1 : 0;
}
