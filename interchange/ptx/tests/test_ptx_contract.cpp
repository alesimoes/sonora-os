// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>

#include "fake_ptx_reader.h"
#include "sonora/interchange/ptx_import.h"

using namespace sonora::interchange;

static int fails = 0;
#define CHECK(c)                                          \
    do {                                                  \
        if (!(c)) {                                       \
            std::printf("FAIL line %d: %s\n", __LINE__, #c); \
            ++fails;                                      \
        }                                                 \
    } while (0)

int main() {
    {
        FakePtxReader r;  // empty document
        PtxImporter imp(&r);
        sonora::model::Snapshot s;
        ConversionReport rep;
        std::string err;
        CHECK(imp.can_read("a.ptx"));
        CHECK(!imp.can_read("a.aaf"));
        CHECK(!imp.read("a.ptx", s, rep, err));
        CHECK(err.find("no tracks or audio") != std::string::npos);
    }
    {
        PtxImporter none(nullptr);
        sonora::model::Snapshot s;
        ConversionReport rep;
        std::string err;
        CHECK(!none.can_read("a.ptx"));
        CHECK(!none.read("a.ptx", s, rep, err));
        CHECK(!err.empty());
    }
    return fails ? 1 : 0;
}
