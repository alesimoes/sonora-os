// SPDX-License-Identifier: GPL-3.0-or-later
//
// editor.h on X11 (Xlib): through Xwayland under labwc on the appliance
// (ADR-0010), WSLg during development, Xvfb in the tests.

#include "editor.h"

#include "ipc.h"
#include "processor.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace sonora::plugin_host {
namespace {

constexpr uint32_t kMaxEditorSide = 16384;  // X11's own limit is 32767
constexpr uint32_t kMinTimerMs = 1, kMaxTimerMs = 60'000;
constexpr std::size_t kWatcherStack = std::size_t(64) << 10;

uint32_t clamp_side(uint32_t v) { return std::clamp<uint32_t>(v, 1u, kMaxEditorSide); }

// Xlib's default handler exits the process on any protocol error: a window
// a plugin already destroyed, a transient-for token naming no window. Not a
// reason to stop a chain that is playing; said and ignored.
int on_x_error(Display* d, XErrorEvent* e) {
    char text[128] = {};
    XGetErrorText(d, e->error_code, text, sizeof text - 1);
    std::fprintf(stderr, "sonora-plugin-host: X11 error ignored: %s (request %d)\n", text, int(e->request_code));
    return 0;
}

// Xlib's default I/O error handler exits the process: an Xwayland that
// restarts would end every host with an editor open, and each of those is
// a chain with its inserts (ADR-0012 §3). The exit handler may return; the
// connection is then dead, every later Xlib call a no-op, and the display
// answers every editor closed.
// The process-wide handler runs first and its default exits; this one
// returns, which hands the decision to the display's exit handler below.
int on_io_error(Display*) { return 0; }

void on_io_error_exit(Display*, void* lost) {
    std::fprintf(stderr, "sonora-plugin-host: the X server went away; closing the plugin editors\n");
    static_cast<std::atomic<bool>*>(lost)->store(true, std::memory_order_release);
}

class X11Display;

class X11Window final : public EditorWindow {
public:
    X11Window(X11Display& display, Display* d, Window w) : display_(display), d_(d), w_(w) {}
    ~X11Window() override;

    uint64_t native_handle() const override { return w_; }
    void set_size(uint32_t width, uint32_t height, bool resizable) override {
        width_ = clamp_side(width);
        height_ = clamp_side(height);
        resizable_ = resizable;
        XSizeHints* hints = XAllocSizeHints();
        if (hints) {
            hints->flags = PSize | PMinSize | (resizable ? 0 : PMaxSize);
            hints->width = hints->min_width = hints->max_width = int(width_);
            hints->height = hints->min_height = hints->max_height = int(height_);
            if (resizable) hints->min_width = hints->min_height = 1;
            XSetWMNormalHints(d_, w_, hints);
            XFree(hints);
        }
        XResizeWindow(d_, w_, width_, height_);
        XFlush(d_);
    }
    void size(uint32_t& width, uint32_t& height) const override {
        width = width_;
        height = height_;
    }
    bool resizable() const override { return resizable_; }
    void show() override {
        XMapRaised(d_, w_);
        XFlush(d_);
    }
    void raise() override {
        XMapRaised(d_, w_);
        XFlush(d_);
    }
    bool close_requested() const override { return close_; }
    bool take_resize(uint32_t& width, uint32_t& height) override {
        if (!resized_) return false;
        resized_ = false;
        width = width_;
        height = height_;
        return true;
    }

    // From X11Display::dispatch.
    void configured(int width, int height) {
        if (width <= 0 || height <= 0) return;
        const uint32_t w = clamp_side(uint32_t(width)), h = clamp_side(uint32_t(height));
        if (w == width_ && h == height_) return;  // ours, or no change
        width_ = w;
        height_ = h;
        resized_ = true;
    }
    void delete_requested() { close_ = true; }

private:
    X11Display& display_;
    Display* d_;
    Window w_;
    uint32_t width_ = 1, height_ = 1;
    bool close_ = false, resized_ = false, resizable_ = false;
};

class X11Display final : public EditorDisplay {
public:
    X11Display(Display* d, int wake_read, int wake_write) : d_(d), wake_read_(wake_read), wake_write_(wake_write) {
        wm_protocols_ = XInternAtom(d_, "WM_PROTOCOLS", False);
        wm_delete_ = XInternAtom(d_, "WM_DELETE_WINDOW", False);
        net_wm_pid_ = XInternAtom(d_, "_NET_WM_PID", False);
        net_wm_name_ = XInternAtom(d_, "_NET_WM_NAME", False);
        utf8_ = XInternAtom(d_, "UTF8_STRING", False);
        XSetIOErrorExitHandler(d_, on_io_error_exit, &lost_);
    }

    bool lost() const override { return lost_.load(std::memory_order_acquire); }

    bool start() {
        watcher_ = start_thread([](void* self) { static_cast<X11Display*>(self)->watch(); }, this, kWatcherStack);
        return watcher_ != nullptr;
    }

    ~X11Display() override {
        if (watcher_) {
            stop_.store(true, std::memory_order_release);
            poke();
            signal(ack_);
            join_thread(watcher_);
        }
        close(wake_read_);
        close(wake_write_);
        XCloseDisplay(d_);
    }

    std::unique_ptr<EditorWindow> create_window(const std::string& title, const std::string& parent_token,
                                                std::string& error) override {
        Window transient = 0;
        if (!parent_token.empty()) {
            char* end = nullptr;
            errno = 0;
            const unsigned long long id = std::strtoull(parent_token.c_str(), &end, 0);
            if (errno != 0 || !end || *end != '\0' || id == 0 || id > 0xFFFFFFFFull) {
                error = "the parent-window token \"" + parent_token.substr(0, 64) + "\" is not an X11 window id";
                return nullptr;
            }
            transient = Window(id);
        }
        const int screen = DefaultScreen(d_);
        XSetWindowAttributes a{};
        a.event_mask = StructureNotifyMask;
        a.background_pixel = BlackPixel(d_, screen);
        const Window w = XCreateWindow(d_, RootWindow(d_, screen), 0, 0, 640, 480, 0, CopyFromParent, InputOutput,
                                       CopyFromParent, CWEventMask | CWBackPixel, &a);
        if (!w) {
            error = "the X server refused a window for the editor";
            return nullptr;
        }
        XStoreName(d_, w, title.c_str());
        XChangeProperty(d_, w, net_wm_name_, utf8_, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(title.data()), int(title.size()));
        // The class is what labwc's window rules match (ADR-0010).
        char res_name[] = "sonora-plugin-editor";
        char res_class[] = "Sonora";
        XClassHint klass{res_name, res_class};
        XSetClassHint(d_, w, &klass);
        Atom protocols[] = {wm_delete_};
        XSetWMProtocols(d_, w, protocols, 1);
        const long pid = long(getpid());
        XChangeProperty(d_, w, net_wm_pid_, XA_CARDINAL, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(&pid), 1);
        if (transient) XSetTransientForHint(d_, w, transient);
        XFlush(d_);
        auto win = std::make_unique<X11Window>(*this, d_, w);
        windows_[w] = win.get();
        return win;
    }

    Id watch_fd(int fd, const void* owner, std::function<void()> ready) override {
        return add(Entry{0, owner, fd, 0, 0, std::move(ready)});
    }
    Id add_timer(uint32_t interval_ms, const void* owner, std::function<void()> fire) override {
        const uint64_t ns = uint64_t(std::clamp(interval_ms, kMinTimerMs, kMaxTimerMs)) * 1'000'000ull;
        return add(Entry{0, owner, -1, ns, monotonic_ns() + ns, std::move(fire)});
    }
    void remove(Id id) override {
        erase_if([&](const Entry& e) { return e.id == id; });
    }
    void remove_fd(const void* owner, int fd) override {
        erase_if([&](const Entry& e) { return e.owner == owner && e.fd == fd && e.interval_ns == 0; });
    }
    void remove_owner(const void* owner) override {
        erase_if([&](const Entry& e) { return e.owner == owner; });
    }
    std::size_t registered(const void* owner) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::size_t(std::count_if(entries_.begin(), entries_.end(),
                                         [&](const Entry& e) { return e.owner == owner; }));
    }

    uint64_t dispatch() override {
        if (lost()) return 0;
        // XPending is what notices a dead server: it reads the socket.
        while (XPending(d_) > 0) {
            XEvent e;
            XNextEvent(d_, &e);
            handle(e);
        }
        // A snapshot: a callback may register or remove entries, its own
        // included, and each one is looked up again before it is called.
        std::vector<pollfd> fds;
        std::vector<Id> fd_ids, timer_ids;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const Entry& e : entries_) {
                if (e.interval_ns) {
                    timer_ids.push_back(e.id);
                } else {
                    fds.push_back(pollfd{e.fd, POLLIN, 0});
                    fd_ids.push_back(e.id);
                }
            }
        }
        if (lost()) return 0;
        if (!fds.empty() && poll(fds.data(), nfds_t(fds.size()), 0) > 0) {
            for (std::size_t i = 0; i < fds.size(); ++i)
                if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) call(fd_ids[i], 0);
        }
        const uint64_t now = monotonic_ns();
        for (Id id : timer_ids) call(id, now);
        XFlush(d_);
        // Callbacks may have queued events of ours (a resize's
        // ConfigureNotify): seen on the next pass, which the watcher wakes.
        signal(ack_);

        uint64_t due = 0;
        std::lock_guard<std::mutex> lock(mutex_);
        for (const Entry& e : entries_)
            if (e.interval_ns && (due == 0 || e.due_ns < due)) due = e.due_ns;
        return due;
    }

    void forget(Window w) {
        windows_.erase(w);
        XDestroyWindow(d_, w);
        XFlush(d_);
    }

private:
    struct Entry {
        Id id;
        const void* owner;
        int fd;                // -1 for a timer
        uint64_t interval_ns;  // 0 for a descriptor
        uint64_t due_ns;
        std::function<void()> fn;
        bool dead = false;  // its descriptor was closed under us (POLLNVAL)
    };

    Id add(Entry e) {
        Id id = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            id = e.id = ++next_id_;
            entries_.push_back(std::move(e));
        }
        poke();
        return id;
    }
    template <class Pred>
    void erase_if(Pred pred) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            entries_.erase(std::remove_if(entries_.begin(), entries_.end(), pred), entries_.end());
        }
        poke();
    }

    // Calls entry `id` if it is still registered: a timer only when due
    // (`now` != 0), rearmed from now, so a late pass fires it once and does
    // not try to catch up.
    void call(Id id, uint64_t now) {
        std::function<void()> fn;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.id == id; });
            if (it == entries_.end()) return;
            if (now) {
                if (now < it->due_ns) return;
                it->due_ns = now + it->interval_ns;
            }
            // A copy: the callback may remove its own entry.
            fn = it->fn;
        }
        fn();
    }

    void handle(const XEvent& e) {
        if (e.type == ClientMessage) {
            const auto it = windows_.find(e.xclient.window);
            if (it != windows_.end() && e.xclient.message_type == wm_protocols_ &&
                Atom(e.xclient.data.l[0]) == wm_delete_)
                it->second->delete_requested();
        } else if (e.type == ConfigureNotify) {
            const auto it = windows_.find(e.xconfigure.window);
            if (it != windows_.end()) it->second->configured(e.xconfigure.width, e.xconfigure.height);
        }
    }

    void poke() {
        const char b = 1;
        while (write(wake_write_, &b, 1) < 0 && errno == EINTR) {
        }
    }

    // The watcher: polls, wakes the main thread, waits for its pass.
    void watch() {
        std::vector<pollfd> fds;
        std::vector<Id> ids;  // ids[i] owns fds[i + 2]
        while (!stop_.load(std::memory_order_acquire)) {
            fds.clear();
            ids.clear();
            fds.push_back(pollfd{wake_read_, POLLIN, 0});
            // A lost connection stays hung up for ever: left in, it would
            // wake this thread in a loop. poll skips a negative fd.
            fds.push_back(pollfd{lost() ? -1 : ConnectionNumber(d_), POLLIN, 0});
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const Entry& e : entries_)
                    if (!e.interval_ns && !e.dead) {
                        fds.push_back(pollfd{e.fd, POLLIN, 0});
                        ids.push_back(e.id);
                    }
            }
            const uint32_t seen = ack_.load(std::memory_order_acquire);
            const int n = poll(fds.data(), nfds_t(fds.size()), -1);
            if (stop_.load(std::memory_order_acquire)) return;
            if (n < 0) continue;  // EINTR
            if (fds[0].revents) {
                char buf[64];
                while (read(wake_read_, buf, sizeof buf) > 0) {
                }
            }
            bool ready = false;
            for (std::size_t i = 1; i < fds.size(); ++i) {
                if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) ready = true;
                // A plugin closed a descriptor it left registered: poll
                // reports it at once, every time. Not a reason to wake
                // anyone, and not polled again; it goes with its editor.
                if ((fds[i].revents & POLLNVAL) && i >= 2) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    for (Entry& e : entries_)
                        if (e.id == ids[i - 2]) e.dead = true;
                }
            }
            if (!ready) continue;
            notify_control();
            while (!stop_.load(std::memory_order_acquire) && ack_.load(std::memory_order_acquire) == seen)
                wait_for_change(ack_, seen, 100'000'000);
        }
    }

    Display* d_;
    int wake_read_, wake_write_;
    Atom wm_protocols_ = 0, wm_delete_ = 0, net_wm_pid_ = 0, net_wm_name_ = 0, utf8_ = 0;
    std::map<Window, X11Window*> windows_;
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    Id next_id_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<bool> lost_{false};
    std::atomic<uint32_t> ack_{0};
    void* watcher_ = nullptr;
};

X11Window::~X11Window() { display_.forget(w_); }

}  // namespace

std::unique_ptr<EditorDisplay> EditorDisplay::open(std::string& error) {
    const char* name = std::getenv("DISPLAY");
    if (!name || !*name) {
        error = "no display to open a plugin editor on (DISPLAY is not set)";
        return nullptr;
    }
    Display* d = XOpenDisplay(nullptr);
    if (!d) {
        error = std::string("cannot open the display \"") + name + "\" for a plugin editor";
        return nullptr;
    }
    XSetErrorHandler(on_x_error);
    XSetIOErrorHandler(on_io_error);
    int wake[2];
    if (pipe2(wake, O_CLOEXEC | O_NONBLOCK) != 0) {
        XCloseDisplay(d);
        error = "cannot make the editor watcher's pipe";
        return nullptr;
    }
    auto display = std::make_unique<X11Display>(d, wake[0], wake[1]);
    if (!display->start()) {
        error = "cannot start the editor watcher thread";
        return nullptr;
    }
    return display;
}

}  // namespace sonora::plugin_host
