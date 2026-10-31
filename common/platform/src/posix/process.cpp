// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/platform/process.h"

#include "region_descriptor.h"

#include <cerrno>
#include <csignal>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

#include <algorithm>
#include <chrono>
#include <thread>

extern char** environ;

namespace sonora::platform {

struct ChildProcess::Impl {
    pid_t pid = -1;
    bool reaped = false;
    int status = 0;
    // It leads its own process group: a signal goes to the group, so what
    // it forked goes with it (a VST3 --list child's per-bundle children,
    // which carry no death signal; code-reviewer B on #95).
    bool group = false;
    // Readable once the child has exited (Linux 5.3 and later): wait()
    // sleeps on it instead of polling waitpid, and an owner with a poll set
    // puts it there (ADR-0012 §4). -1 where the kernel has none; wait()
    // then falls back to polling.
    int exit_fd = -1;

    void signal(int sig) const { ::kill(group ? -pid : pid, sig); }

    bool reap(bool block) {
        if (reaped) return true;
        int st = 0;
        pid_t r;
        do {
            r = ::waitpid(pid, &st, block ? 0 : WNOHANG);
        } while (r < 0 && errno == EINTR);
        if (r == pid) {
            reaped = true;
            status = st;
        } else if (r < 0) {
            // Someone else reaped it (a SIGCHLD set to ignore, say): gone.
            reaped = true;
            status = 0;
        }
        return reaped;
    }

    enum class ExitFd { Exited, TimedOut, Unusable };
    // Exited only on POLLIN: that is the one answer that means a zombie
    // waits, so the blocking reap after it cannot hang. POLLERR, POLLHUP or
    // POLLNVAL alone say nothing about the child, and a poll that fails
    // says less; the caller polls waitpid instead (#183 review).
    ExitFd exit_fd_readable(std::chrono::steady_clock::time_point end) const {
        if (exit_fd < 0) return ExitFd::Unusable;
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                end - std::chrono::steady_clock::now());
            pollfd p{exit_fd, POLLIN, 0};
            const int r = ::poll(&p, 1, static_cast<int>(std::max<int64_t>(0, left.count())));
            if (r > 0) return (p.revents & POLLIN) ? ExitFd::Exited : ExitFd::Unusable;
            if (r == 0) return ExitFd::TimedOut;
            if (errno != EINTR) return ExitFd::Unusable;
        }
    }
};

namespace {

#ifdef __linux__
#ifndef SYS_pidfd_open
// The same number on every architecture (new syscalls share one table);
// older libc headers lack it.
#define SYS_pidfd_open 434
#endif
#endif

// Close-on-exec by the kernel's own rule, so no other child inherits it.
// The child is ours and unreaped, so its pid cannot have been reused, even
// if it has already exited (a zombie has a pidfd too).
int open_exit_fd(pid_t pid) {
#ifdef __linux__
    const long fd = ::syscall(SYS_pidfd_open, pid, 0);
    return fd < 0 ? -1 : static_cast<int>(fd);
#else
    (void)pid;
    return -1;
#endif
}

}  // namespace

std::string executable_dir() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return {};
    std::string path(buf, static_cast<std::size_t>(n));
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

int64_t current_process_id() { return static_cast<int64_t>(::getpid()); }

namespace {

#ifdef __linux__
// <linux/ioprio.h> is not in every libc's headers; the values are the ABI.
constexpr int kIoprioWhoProcess = 1;
constexpr int kIoprioClassIdle = 3;
constexpr int kIoprioClassShift = 13;
#endif

// `who`: a thread id (Linux) or process id; 0 is the caller.
bool lower(pid_t who, int nice, bool idle_io, std::string& error) {
    bool ok = true;
    if (nice > 0) {
        errno = 0;
        const int now = ::getpriority(PRIO_PROCESS, static_cast<id_t>(who));
        if (errno == 0 && now < nice && ::setpriority(PRIO_PROCESS, static_cast<id_t>(who), nice) != 0) {
            error = std::string("cannot lower the CPU priority: ") + std::strerror(errno);
            ok = false;
        }
    }
    if (idle_io) {
#ifdef __linux__
        if (::syscall(SYS_ioprio_set, kIoprioWhoProcess, static_cast<int>(who),
                      kIoprioClassIdle << kIoprioClassShift) != 0) {
            error = std::string("cannot set the idle I/O class: ") + std::strerror(errno);
            ok = false;
        }
#endif
    }
    return ok;
}

}  // namespace

bool lower_current_thread(int nice, bool idle_io, std::string& error) {
#ifdef __linux__
    // Linux keeps both per thread: setpriority and ioprio_set on a thread
    // id touch that thread alone, and a child inherits its spawner's.
    const pid_t self = static_cast<pid_t>(::syscall(SYS_gettid));
    return lower(self, nice, idle_io, error);
#else
    // Elsewhere POSIX's setpriority would lower the whole process, the
    // engine's control and disk threads with it (rt-engineer on #95).
    (void)nice;
    (void)idle_io;
    error = "lowering one thread's priority is not available on this platform";
    return false;
#endif
}

std::unique_ptr<ChildProcess> ChildProcess::spawn(const std::vector<std::string>& argv,
                                                  const Options& options, std::string& error) {
    if (argv.empty()) {
        error = "nothing to run";
        return nullptr;
    }
    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);

    // Each passed region or handle is first duplicated, close-on-exec, at or
    // above kStagingFloor, so that one dup2 into place cannot overwrite the
    // source of another; the dup2 in the child clears close-on-exec on the
    // target only. A target at or above the floor could be a staged source,
    // and two entries with one target would silently overwrite each other:
    // both are refused (rt-engineer L5 on #78).
    constexpr int kStagingFloor = 64;
    std::vector<int> staged;
    auto close_staged = [&staged] {
        for (int fd : staged) ::close(fd);
    };
    {
        std::vector<int> targets;
        for (const PassedRegion& r : options.regions) targets.push_back(r.child_fd);
        for (const Options::PassedHandle& h : options.handles) targets.push_back(h.child_fd);
        std::sort(targets.begin(), targets.end());
        for (std::size_t i = 0; i < targets.size(); ++i) {
            if (targets[i] >= kStagingFloor) {
                error = "cannot pass a descriptor to " + argv[0] + " at " + std::to_string(targets[i]) +
                        ": targets stay below " + std::to_string(kStagingFloor);
                return nullptr;
            }
            if (i > 0 && targets[i] == targets[i - 1]) {
                error = "cannot pass two descriptors to " + argv[0] + " at " + std::to_string(targets[i]);
                return nullptr;
            }
        }
    }
    for (const PassedRegion& r : options.regions) {
        const int fd = r.region ? region_descriptor(*r.region, r.writable) : -1;
        if (r.child_fd < 3 || fd < 0) {
            error = "cannot pass a region to " + argv[0] + " at descriptor " +
                    std::to_string(r.child_fd) +
                    (fd < 0 ? ": the region has no such descriptor" : ": 0-2 are the streams");
            close_staged();
            return nullptr;
        }
        const int high = ::fcntl(fd, F_DUPFD_CLOEXEC, kStagingFloor);
        if (high < 0) {
            error = "cannot duplicate a region descriptor: " + std::string(std::strerror(errno));
            close_staged();
            return nullptr;
        }
        staged.push_back(high);
    }
    // Raw handles the same way, after the regions.
    for (const Options::PassedHandle& h : options.handles) {
        if (h.child_fd < 3 || h.handle < 0) {
            error = "cannot pass handle " + std::to_string(h.handle) + " to " + argv[0] + " at descriptor " +
                    std::to_string(h.child_fd) + (h.handle < 0 ? ": no handle" : ": 0-2 are the streams");
            close_staged();
            return nullptr;
        }
        const int high = ::fcntl(h.handle, F_DUPFD_CLOEXEC, kStagingFloor);
        if (high < 0) {
            error = "cannot duplicate a handle: " + std::string(std::strerror(errno));
            close_staged();
            return nullptr;
        }
        staged.push_back(high);
    }

    posix_spawn_file_actions_t actions;
    int fa = posix_spawn_file_actions_init(&actions);
    if (fa != 0) {
        error = "cannot prepare to start " + argv[0] + ": " + std::strerror(fa);
        close_staged();
        return nullptr;
    }
    for (std::size_t i = 0; i < staged.size() && fa == 0; ++i) {
        const int target = i < options.regions.size() ? options.regions[i].child_fd
                                                       : options.handles[i - options.regions.size()].child_fd;
        fa = posix_spawn_file_actions_adddup2(&actions, staged[i], target);
    }
    if (fa == 0 && !options.stdout_path.empty()) {
        fa = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, options.stdout_path.c_str(),
                                              O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    if (fa == 0 && !options.stderr_path.empty()) {
        fa = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, options.stderr_path.c_str(),
                                              O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    if (fa != 0) {
        error = "cannot prepare to start " + argv[0] + ": " + std::strerror(fa);
        posix_spawn_file_actions_destroy(&actions);
        close_staged();
        return nullptr;
    }
    // The child starts with default signal handling and nothing blocked,
    // whatever its parent ignores or masks.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, defaults;
    sigemptyset(&none);
    sigemptyset(&defaults);
    for (int s : {SIGPIPE, SIGINT, SIGTERM, SIGHUP, SIGCHLD}) sigaddset(&defaults, s);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
    if (options.own_group) {
        posix_spawnattr_setpgroup(&attr, 0);
        flags |= POSIX_SPAWN_SETPGROUP;
    }
    posix_spawnattr_setflags(&attr, flags);

    auto impl = std::make_unique<Impl>();
    impl->group = options.own_group;
    const int rc = ::posix_spawn(&impl->pid, args[0], &actions, &attr, args.data(), environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    close_staged();
    if (rc != 0) {
        error = "cannot start " + argv[0] + ": " + std::strerror(rc);
        return nullptr;
    }
    if (options.nice > 0 || options.idle_io) {
        // posix_spawn has no attribute for either. A spawner that lowered
        // itself first (lower_current_thread) passed both on already; this
        // covers any other, from just after exec. A refusal is not a failed
        // start: the child runs, at the priority it inherited.
        std::string ignored;
        lower(impl->pid, options.nice, options.idle_io, ignored);
    }
    impl->exit_fd = open_exit_fd(impl->pid);
    return std::make_unique<ChildProcess>(std::move(impl));
}

ChildProcess::ChildProcess(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

ChildProcess::~ChildProcess() {
    if (!impl_->reap(false)) {
        impl_->signal(SIGKILL);
        impl_->reap(true);
    }
    if (impl_->exit_fd >= 0) ::close(impl_->exit_fd);
}

bool ChildProcess::running() { return !impl_->reap(false); }

int64_t ChildProcess::pid() const { return static_cast<int64_t>(impl_->pid); }

int ChildProcess::exit_handle() const { return impl_->exit_fd; }

bool ChildProcess::wait(int timeout_ms) {
    if (timeout_ms < 0) return impl_->reap(true);
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    if (impl_->exit_fd >= 0) {
        if (impl_->reap(false)) return true;
        switch (impl_->exit_fd_readable(end)) {
            // A zombie waits, so this reap does not block.
            case Impl::ExitFd::Exited: return impl_->reap(true);
            case Impl::ExitFd::TimedOut: return false;
            case Impl::ExitFd::Unusable: break;  // the rest of the bound, polled
        }
    }
    for (;;) {
        if (impl_->reap(false)) return true;
        if (std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void ChildProcess::terminate() {
    if (!impl_->reaped) impl_->signal(SIGTERM);
}

void ChildProcess::kill() {
    if (!impl_->reaped) impl_->signal(SIGKILL);
}

bool ChildProcess::exited_normally() const { return WIFEXITED(impl_->status); }
int ChildProcess::exit_code() const { return WEXITSTATUS(impl_->status); }
int ChildProcess::signal_number() const {
    return WIFSIGNALED(impl_->status) ? WTERMSIG(impl_->status) : 0;
}

}  // namespace sonora::platform
