// SPDX-License-Identifier: GPL-3.0-or-later
#include "loader.h"

#include "ipc.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace sonora::plugin_host {

void* open_library(const std::string& path, std::string& error) {
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* why = dlerror();
        error = why ? why : ("cannot load " + path);
    }
    return h;
}

void* library_symbol(void* library, const char* name) { return dlsym(library, name); }

void close_library(void* library) {
    if (library) dlclose(library);
}

bool bind_native_telemetry(void* library, void* block) {
    if (library == nullptr || block == nullptr) return false;
    using Bind = int (*)(void*);
    // dlsym returns an object pointer; the cast is the loader's own idiom.
    const Bind bind = reinterpret_cast<Bind>(dlsym(library, "sonora_native_telemetry_bind"));
    return bind != nullptr && bind(block) != 0;
}

bool bind_native_telemetry_at(const std::string& path, void* block) {
    if (path.empty() || block == nullptr) return false;
    // RTLD_NOLOAD: only a library that is already mapped, so a path that is
    // not the plugin's is never loaded for this.
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
    if (h == nullptr) return false;
    const bool bound = bind_native_telemetry(h, block);
    dlclose(h);
    return bound;
}

std::vector<std::string> vst3_default_roots() {
    std::vector<std::string> roots;
    const char* home = std::getenv("HOME");
    if (!home || !*home) {
        if (const passwd* pw = getpwuid(getuid())) home = pw->pw_dir;
    }
    if (home && *home) roots.push_back(std::string(home) + "/.vst3");
    roots.push_back("/usr/lib/vst3");
    roots.push_back("/usr/local/lib/vst3");
    return roots;
}

const char* vst3_architecture() {
#if defined(__x86_64__)
    return "x86_64-linux";
#elif defined(__aarch64__)
    return "aarch64-linux";
#elif defined(__i386__)
    return "i386-linux";
#elif defined(__arm__)
    return "armv7l-linux";
#else
    return "unknown-linux";
#endif
}

IsolatedResult run_isolated(const std::function<int(std::FILE* out)>& body, uint64_t timeout_ns) {
    IsolatedResult r;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
        r.output = "";
        r.exit_code = 127;
        r.exited = true;
        return r;
    }
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        r.exited = true;
        r.exit_code = 127;
        return r;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(STDERR_FILENO, STDOUT_FILENO);
        std::FILE* out = fdopen(fds[1], "w");
        if (!out) _exit(127);
        // Fully buffered and large, so a record reaches the pipe in one
        // piece when the body flushes after it, not in BUFSIZ fragments.
        setvbuf(out, nullptr, _IOFBF, 1u << 20);
        const int code = body(out);
        std::fflush(out);
        std::fflush(stdout);
        // Not exit(): the child must not run the parent's atexit handlers or
        // a plugin's static destructors a second time.
        _exit(code);
    }
    close(fds[1]);
    const uint64_t end = monotonic_ns() + timeout_ns;
    char buf[4096];
    for (;;) {
        const uint64_t now = monotonic_ns();
        if (now >= end) {
            r.timed_out = true;
            kill(pid, SIGKILL);
            break;
        }
        pollfd p{fds[0], POLLIN, 0};
        const int ms = static_cast<int>(std::min<uint64_t>((end - now) / 1000000 + 1, 1000));
        const int n = poll(&p, 1, ms);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) continue;
        const ssize_t got = read(fds[0], buf, sizeof buf);
        if (got > 0) {
            r.output.append(buf, static_cast<std::size_t>(got));
            continue;
        }
        if (got < 0 && errno == EINTR) continue;
        break;  // EOF: the child closed stdout, normally by exiting
    }
    close(fds[0]);
    // EOF is not exit: a child that closed the pipe (or handed it to a
    // grandchild and stopped writing) can still hang, so the wait has the
    // same deadline as the reads.
    int status = 0;
    for (;;) {
        const pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
        if (w < 0 && errno != EINTR) break;
        if (monotonic_ns() >= end) {
            r.timed_out = true;
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            break;
        }
        usleep(2000);
    }
    // A child killed in the middle of a record leaves part of a line behind;
    // the caller appends its own record after this, so only whole lines stay.
    if (const std::size_t nl = r.output.rfind('\n'); nl == std::string::npos)
        r.output.clear();
    else
        r.output.resize(nl + 1);
    if (WIFEXITED(status)) {
        r.exited = !r.timed_out;
        r.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        r.signal = WTERMSIG(status);
    }
    return r;
}

}  // namespace sonora::plugin_host
