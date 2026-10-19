// SPDX-License-Identifier: GPL-3.0-or-later
// What the plugin scanner asks of the OS seam (ADR-0012, amendment of
// 2026-10-19, changes A, B and D): the per-user cache folder, a path's
// status through a symlink with its change time, a thread lowered to nice
// 10 and the idle I/O class without touching its siblings, and a child
// started lowered, both by the spawn option and by inheritance.
#include "sonora/platform/file.h"
#include "sonora/platform/process.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

using namespace sonora::platform;

namespace {

int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

void cache_dir() {
    const char* home = std::getenv("HOME");
    const std::string saved_home = home ? home : "";
    ::setenv("XDG_CACHE_HOME", "/var/cache/x", 1);
    CHECK(user_cache_dir() == "/var/cache/x/sonora");
    // Relative: invalid under the XDG rule, so HOME's.
    ::setenv("XDG_CACHE_HOME", "relative/cache", 1);
    ::setenv("HOME", "/home/op", 1);
    CHECK(user_cache_dir() == "/home/op/.cache/sonora");
    ::unsetenv("XDG_CACHE_HOME");
    CHECK(user_cache_dir() == "/home/op/.cache/sonora");
    ::unsetenv("HOME");
    CHECK(user_cache_dir().empty());
    if (home) ::setenv("HOME", saved_home.c_str(), 1);
}

void status_through_symlink() {
    const std::string dir = "test_platform_background";
    std::system(("rm -rf " + dir).c_str());
    ::mkdir(dir.c_str(), 0755);
    {
        std::ofstream f(dir + "/target.so");
        f << "0123456789";
    }
    CHECK(::symlink("target.so", (dir + "/link.so").c_str()) == 0);
    PathStatus s;
    CHECK(path_status(dir + "/link.so", s));
    CHECK(s.regular && !s.directory && s.size == 10);
    CHECK(s.modified_ns > 0 && s.changed_ns > 0);
    // touch -d sets the mtime back; the ctime still moves forward.
    const int64_t ctime_before = s.changed_ns;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::system(("touch -d 2001-01-01 " + dir + "/target.so").c_str());
    PathStatus after;
    CHECK(path_status(dir + "/link.so", after));
    CHECK(after.modified_ns < s.modified_ns);
    CHECK(after.changed_ns > ctime_before);
    CHECK(path_status(dir, s) && s.directory);
    CHECK(!path_status(dir + "/absent", s));
    ::unlink((dir + "/target.so").c_str());
    CHECK(!path_status(dir + "/link.so", s));  // dangling
    std::system(("rm -rf " + dir).c_str());
}

#ifdef __linux__
int thread_nice() { return ::getpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid))); }
int thread_io_class() { return int(::syscall(SYS_ioprio_get, 1, int(::syscall(SYS_gettid))) >> 13); }

// Field 19 of /proc/<pid>/stat is the nice value.
int process_nice(int64_t pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
    std::string s;
    std::getline(f, s);
    const auto close = s.rfind(')');
    if (close == std::string::npos) return -100;
    int field = 3;
    std::size_t at = close + 1;
    while (field < 19 && at < s.size()) {
        at = s.find(' ', at + 1);
        ++field;
    }
    return std::atoi(s.c_str() + at + 1);
}

void lowered() {
    const int main_nice = thread_nice();
    int worker_nice = -100, worker_io = -1, child_nice = -100;
    bool ok = false;
    std::thread worker([&] {
        std::string e;
        ok = lower_current_thread(10, true, e);
        if (!ok) std::printf("  lower_current_thread: %s\n", e.c_str());
        worker_nice = thread_nice();
        worker_io = thread_io_class();
        // A child of the lowered thread starts lowered.
        std::string error;
        auto c = ChildProcess::spawn({"/bin/sleep", "1"}, {}, error);
        if (c) {
            child_nice = process_nice(c->pid());
            c->kill();
        }
    });
    worker.join();
    CHECK(ok);
    CHECK(worker_nice == 10);
    CHECK(worker_io == 3);       // IOPRIO_CLASS_IDLE
    CHECK(child_nice == 10);
    CHECK(thread_nice() == main_nice);  // the caller's other threads untouched

    // The spawn option alone, from a thread that was not lowered.
    ChildProcess::Options o;
    o.nice = 10;
    o.idle_io = true;
    std::string error;
    auto c = ChildProcess::spawn({"/bin/sleep", "1"}, o, error);
    CHECK(c != nullptr);
    if (c) {
        CHECK(process_nice(c->pid()) == 10);
        c->kill();
    }
}
#endif

}  // namespace

int main() {
    cache_dir();
    status_through_symlink();
#ifdef __linux__
    lowered();
#endif
    if (failures) {
        std::printf("test_platform_background: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_platform_background: ok\n");
    return 0;
}
