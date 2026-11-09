// SPDX-License-Identifier: GPL-3.0-or-later
//
// The elastic copy while it is being written (ADR-0136 §5): closed and
// removed when the scope ends without keep(), so a throw (std::bad_alloc in
// the block buffer) leaves no partial file in Audio Files, as a failure or a
// cancel already did not. Worker thread only.
#pragma once

#include <cstdio>
#include <string>
#include <utility>

#include "sonora/wav_writer.h"

namespace sonora::elastic {

class PartialCopy {
public:
    PartialCopy(WavWriter& writer, std::string path)
        : writer_(writer), path_(std::move(path)) {}
    ~PartialCopy() {
        if (path_.empty()) return;
        // Closed first: a file still open cannot be removed on every platform.
        writer_.close();
        std::remove(path_.c_str());
    }

    PartialCopy(const PartialCopy&) = delete;
    PartialCopy& operator=(const PartialCopy&) = delete;

    // The copy is complete and described: from here the caller owns it.
    void keep() { path_.clear(); }

private:
    WavWriter& writer_;
    std::string path_;
};

}  // namespace sonora::elastic
