// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>

#include "fake_omf_reader.h"
#include "sonora/interchange/omf_import.h"

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
    FakeOmfReader fake;  // empty document
    OmfImporter imp(&fake);
    CHECK(imp.can_read("a.omf"));
    sonora::model::Snapshot snap;
    ConversionReport rep;
    std::string err;
    CHECK(!imp.read("a.omf", snap, rep, err));
    CHECK(err.find("no tracks") != std::string::npos);

    fake.ok = false;
    err.clear();
    CHECK(!imp.read("a.omf", snap, rep, err));
    CHECK(err == "fake reader failure");

    OmfImporter none(nullptr);
    CHECK(!none.can_read("a.omf"));
    err.clear();
    CHECK(!none.read("a.omf", snap, rep, err));
    CHECK(!err.empty());
    return fails ? 1 : 0;
}
