// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/platform/process.h"

#include <cerrno>
#include <csignal>
#include <cstring>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <thread>

extern char** environ;

namespace sonora::platform {

struct ChildProcess::Impl {
    pid_t pid = -1;
    bool reaped = false;
    int status = 0;

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
};

std::string executable_dir() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return {};
    std::string path(buf, static_cast<std::size_t>(n));
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
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

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (!options.stdout_path.empty()) {
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, options.stdout_path.c_str(),
                                         O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    if (!options.stderr_path.empty()) {
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, options.stderr_path.c_str(),
                                         O_WRONLY | O_CREAT | O_TRUNC, 0644);
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
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    auto impl = std::make_unique<Impl>();
    const int rc = ::posix_spawn(&impl->pid, args[0], &actions, &attr, args.data(), environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) {
        error = "cannot start " + argv[0] + ": " + std::strerror(rc);
        return nullptr;
    }
    return std::make_unique<ChildProcess>(std::move(impl));
}

ChildProcess::ChildProcess(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

ChildProcess::~ChildProcess() {
    if (!impl_->reap(false)) {
        ::kill(impl_->pid, SIGKILL);
        impl_->reap(true);
    }
}

bool ChildProcess::running() { return !impl_->reap(false); }

bool ChildProcess::wait(int timeout_ms) {
    if (timeout_ms < 0) return impl_->reap(true);
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (impl_->reap(false)) return true;
        if (std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void ChildProcess::terminate() {
    if (!impl_->reaped) ::kill(impl_->pid, SIGTERM);
}

void ChildProcess::kill() {
    if (!impl_->reaped) ::kill(impl_->pid, SIGKILL);
}

bool ChildProcess::exited_normally() const { return WIFEXITED(impl_->status); }
int ChildProcess::exit_code() const { return WEXITSTATUS(impl_->status); }
int ChildProcess::signal_number() const {
    return WIFSIGNALED(impl_->status) ? WTERMSIG(impl_->status) : 0;
}

}  // namespace sonora::platform
