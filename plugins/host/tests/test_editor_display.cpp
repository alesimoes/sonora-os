// SPDX-License-Identifier: GPL-3.0-or-later
//
// EditorDisplay (src/editor.h) against a private Xvfb this test starts and,
// in the second case, kills:
//   dead_fd   a plugin closes a descriptor it left registered: poll answers
//             POLLNVAL at once, for ever; the watcher must not spin on it.
//   x_gone    the X server dies (an Xwayland restart): the display reports
//             lost() and the process lives; Xlib's default exits it.

#include "editor.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// The host's main loop sleeps on a word the watcher wakes; this test has no
// such loop and drives dispatch() itself.
namespace sonora::plugin_host {
void notify_control() noexcept {}
}  // namespace sonora::plugin_host

using namespace sonora::plugin_host;

namespace {

int g_failed = 0;
void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}

double cpu_seconds() {
    timespec ts{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
}

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Xvfb on a display number it picks itself (-displayfd); sets DISPLAY.
struct Xvfb {
    pid_t pid = -1;
    bool start() {
        int fds[2];
        if (pipe(fds) != 0) return false;
        pid = fork();
        if (pid < 0) return false;
        if (pid == 0) {
            close(fds[0]);
            const std::string fd = std::to_string(fds[1]);
            execlp("Xvfb", "Xvfb", "-displayfd", fd.c_str(), "-screen", "0", "640x480x24", "-nolisten", "tcp",
                   static_cast<char*>(nullptr));
            _exit(127);
        }
        close(fds[1]);
        char buf[32] = {};
        std::size_t got = 0;
        while (got < sizeof buf - 1) {
            const ssize_t n = read(fds[0], buf + got, sizeof buf - 1 - got);
            if (n <= 0) break;
            got += std::size_t(n);
            if (buf[got - 1] == '\n') break;
        }
        close(fds[0]);
        if (got == 0) return false;
        const std::string display = ":" + std::string(buf, got);
        setenv("DISPLAY", display.substr(0, display.find('\n')).c_str(), 1);
        return true;
    }
    void stop() {
        if (pid <= 0) return;
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        pid = -1;
    }
    ~Xvfb() { stop(); }
};

void dead_fd() {
    std::printf("dead_fd: a registered descriptor is closed under the watcher\n");
    std::string error;
    auto display = EditorDisplay::open(error);
    check(display != nullptr, "the display opens");
    if (!display) return;
    int p[2];
    check(pipe2(p, O_CLOEXEC) == 0, "a pipe");
    int calls = 0;
    const void* owner = &calls;
    display->watch_fd(p[0], owner, [&] { ++calls; });
    sleep_ms(100);  // the watcher has polled it once
    close(p[0]);    // left registered
    close(p[1]);
    const double before = cpu_seconds();
    sleep_ms(500);
    const double used = cpu_seconds() - before;
    std::printf("  %.3f s of CPU in 0.5 s with the descriptor dead\n", used);
    check(used < 0.1, "the watcher does not spin on POLLNVAL");
    check(calls == 0, "and the dead descriptor's callback is never called");
    display->dispatch();
    check(display->registered(owner) == 1, "the entry stays until its owner removes it");
    display->remove_owner(owner);
    check(display->registered(owner) == 0, "and goes with its owner");
}

void x_gone(Xvfb& x) {
    std::printf("x_gone: the X server is killed under an open display\n");
    std::string error;
    auto display = EditorDisplay::open(error);
    check(display != nullptr, "the display opens");
    if (!display) return;
    auto window = display->create_window("test", "", error);
    check(window != nullptr, "a window");
    check(!display->lost(), "not lost while the server lives");
    x.stop();
    bool lost = false;
    for (int i = 0; i < 100 && !lost; ++i) {
        display->dispatch();
        lost = display->lost();
        if (!lost) sleep_ms(20);
    }
    // Reaching here is the point: Xlib's default handler exits the process.
    check(lost, "dispatch() notices: lost(), and the process is still alive");
    const double before = cpu_seconds();
    sleep_ms(500);
    for (int i = 0; i < 5; ++i) display->dispatch();
    const double used = cpu_seconds() - before;
    std::printf("  %.3f s of CPU in 0.5 s with the server gone\n", used);
    check(used < 0.1, "the watcher does not spin on the hung-up connection");
    window.reset();
    display.reset();
    check(true, "the windows and the display are destroyed without exiting");
}

}  // namespace

int main(int argc, char** argv) {
    ::signal(SIGPIPE, SIG_IGN);
    const std::string which = argc > 1 ? argv[1] : "";
    Xvfb x;
    if (!x.start()) {
        std::printf("SKIP: no Xvfb to start\n");
        return 77;
    }
    if (which == "dead_fd") dead_fd();
    else if (which == "x_gone") x_gone(x);
    else {
        std::printf("usage: test_editor_display dead_fd|x_gone\n");
        return 2;
    }
    std::printf("%s\n", g_failed ? "FAILED" : "PASSED");
    return g_failed ? 1 : 0;
}
