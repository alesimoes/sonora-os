// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/console_probe.h"

#include <cstring>

#include "sonora/console_keys.h"

#ifdef __linux__
#include <fcntl.h>
#include <linux/kd.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace sonora::tui {

bool is_linux_console(int fd, const char* term_env, const KeyboardTypeProbe& probe) {
    if (term_env == nullptr || std::strcmp(term_env, "linux") != 0) return false;
    return probe && probe(fd);
}

std::string read_dumpkeys(const DumpkeysRunner& runner) {
    std::string out;
    if (!runner || !runner(out)) return {};
    return out;
}

ConsoleChords chords_for_console(const std::vector<KeyChord>& xterm, bool on_console,
                                 const DumpkeysRunner& runner) {
    ConsoleChords result;
    if (!on_console) {
        result.chords = xterm;
        return result;
    }
    ConsoleKeymap table = parse_console_keymap(read_dumpkeys(runner), nullptr);
    if (table.empty()) {
        table = builtin_console_keymap();
        result.notice =
            "console keymap unreadable (dumpkeys missing or empty): using the default Linux"
            " table, keys a remapped console changes may differ";
    }
    result.chords = filter_console_chords(xterm, table);
    return result;
}

#ifdef __linux__
KeyboardTypeProbe real_keyboard_type_probe() {
    return [](int fd) {
        char type = 0;
        return ::ioctl(fd, KDGKBTYPE, &type) == 0;
    };
}

DumpkeysRunner real_dumpkeys_runner() {
    return [](std::string& out) {
        constexpr std::size_t kMaxBytes = 1 << 20;
        constexpr int kTimeoutMs = 3000;
        int fds[2];
        if (::pipe(fds) != 0) return false;
        const pid_t pid = ::fork();
        if (pid < 0) {
            ::close(fds[0]);
            ::close(fds[1]);
            return false;
        }
        if (pid == 0) {
            ::dup2(fds[1], STDOUT_FILENO);
            const int null = ::open("/dev/null", O_WRONLY);
            if (null >= 0) ::dup2(null, STDERR_FILENO);
            ::close(fds[0]);
            ::close(fds[1]);
            ::execl("/usr/bin/dumpkeys", "dumpkeys", static_cast<char*>(nullptr));
            ::_exit(127);
        }
        ::close(fds[1]);
        bool complete = false;
        char buf[4096];
        for (;;) {
            pollfd p{fds[0], POLLIN, 0};
            if (::poll(&p, 1, kTimeoutMs) <= 0) break;
            const ssize_t n = ::read(fds[0], buf, sizeof buf);
            if (n < 0) break;
            if (n == 0) {
                complete = true;
                break;
            }
            out.append(buf, static_cast<std::size_t>(n));
            if (out.size() > kMaxBytes) break;
        }
        ::close(fds[0]);
        if (!complete) ::kill(pid, SIGKILL);
        int status = 0;
        ::waitpid(pid, &status, 0);
        return complete && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    };
}
#else
KeyboardTypeProbe real_keyboard_type_probe() {
    return [](int) { return false; };
}
DumpkeysRunner real_dumpkeys_runner() {
    return [](std::string&) { return false; };
}
#endif

}  // namespace sonora::tui
