// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Time Compression/Expansion dialog's logic (ADR-0175): refuses a ratio
// outside the elastic limits before anything is sent, then sends
// `elastic ratio R ALGO` for the selected region through the Warp panel's own
// command path (elastic_command.h). Pitch stays the separate pitch verb.
#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "sonora/ui_elastic/elastic_command.h"

namespace sonora::ui::tce {

class TceDialog {
public:
    // `source_frames`: the selected region's length when the dialog opened.
    TceDialog(EditController& ec, ElasticDeps deps, int64_t source_frames)
        : ec_(ec), deps_(std::move(deps)), source_(source_frames) {}

    // Sends; false with `why` (a held notice) when refused, nothing sent.
    bool apply(double ratio, const std::string& algorithm, std::string& why);

    // The length shown after a successful apply, 0 before one. The send path
    // carries no reply body, so it is the accepted ratio's length.
    int64_t resulting_length() const { return result_; }

private:
    EditController& ec_;
    ElasticDeps deps_;
    int64_t source_;
    int64_t result_ = 0;
};

}  // namespace sonora::ui::tce
