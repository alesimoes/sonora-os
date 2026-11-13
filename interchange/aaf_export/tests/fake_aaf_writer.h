// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include "sonora/interchange/aaf_export.h"

// Captures the document it is given; fails on demand.
class FakeAafWriter : public sonora::interchange::AafWriter {
public:
    bool write(const sonora::interchange::AafDocument& d, const std::string& p,
               std::string& error) override {
        doc = d;
        path = p;
        ++calls;
        if (fail) error = "fake writer failure";
        return !fail;
    }
    sonora::interchange::AafDocument doc;
    std::string path;
    int calls = 0;
    bool fail = false;
};
