// SPDX-License-Identifier: GPL-3.0-or-later
#include "edit_loop_handle.h"

namespace sonora::ui {

int loop_handle_repeats(std::int64_t length, std::int64_t past) {
    if (length <= 0) return 0;
    const double lengths = static_cast<double>(past) / static_cast<double>(length);
    return lengths < 0.5 ? 0 : static_cast<int>(lengths + 0.5);
}

}  // namespace sonora::ui
