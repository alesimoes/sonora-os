// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "sonora/interchange/omf_document.h"

namespace sonora::interchange {

class FakeOmfReader : public OmfReader {
public:
    OmfDocument doc;
    bool ok = true;
    std::string fail_with = "fake reader failure";
    bool read(const std::string&, OmfDocument& out, std::string& error) override {
        if (!ok) {
            error = fail_with;
            return false;
        }
        out = doc;
        return true;
    }
};

}  // namespace sonora::interchange
