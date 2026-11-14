// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "sonora/interchange/aaf_document.h"

namespace sonora::interchange {

class FakeAafReader : public AafReader {
public:
    AafDocument doc;
    bool ok = true;
    std::string fail_with = "fake reader failure";
    bool read(const std::string&, AafDocument& out, std::string& error) override {
        if (!ok) {
            error = fail_with;
            return false;
        }
        out = doc;
        return true;
    }
};

}  // namespace sonora::interchange
