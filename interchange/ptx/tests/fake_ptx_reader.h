// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include "sonora/interchange/ptx_document.h"

namespace sonora::interchange {

class FakePtxReader : public PtxReader {
public:
    PtxDocument doc;
    bool succeed = true;
    std::string fail_with = "fake failure";

    bool read(const std::string&, PtxDocument& out, std::string& error) override {
        if (!succeed) {
            error = fail_with;
            return false;
        }
        out = doc;
        return true;
    }
};

}  // namespace sonora::interchange
