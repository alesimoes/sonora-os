// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/exit_watchdog.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace sonora {

void arm_exit_watchdog(unsigned seconds, const char* message) {
    static std::atomic<bool> armed{false};
    if (armed.exchange(true)) return;
    std::thread([seconds, message] {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        // write() and _exit() only: the thread it interrupts may hold any
        // lock, stdio's included.
        const ssize_t ignored = ::write(STDERR_FILENO, message, std::strlen(message));
        (void)ignored;
        ::_exit(1);
    }).detach();
}

}  // namespace sonora
