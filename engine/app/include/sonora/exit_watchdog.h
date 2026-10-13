// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace sonora {

// Bounds the rest of the process's life. After `seconds`, unless it has
// exited by then, writes `message` to stderr and ends with _exit(1),
// running no destructors. For teardown that can block on a wedged audio
// server once the take is already closed. Only the first call counts.
// `message` must outlive the process (a string literal).
void arm_exit_watchdog(unsigned seconds, const char* message);

}  // namespace sonora
