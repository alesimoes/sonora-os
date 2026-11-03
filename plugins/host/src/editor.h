// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Plugin editor windows, the host's side (contracts/plugin host_control.h,
// "Editors"; ADR-0010: parentless X11 toplevels that labwc places). Neutral
// here; the X11 implementation is src/linux/editor_x11.cpp, and a build
// without X11 has src/editor_none.cpp, which refuses every editor.
//
// Everything here runs on the host's MAIN thread: the one that created the
// plugins and serves HostControl. VST3 asks for its view calls there, and the
// processing thread never sees any of it, so an editor that redraws slowly,
// hangs or loops costs its own window and never a block. An editor that
// crashes takes this process with it, which the engine already treats as a
// plugin crash (ADR-0012 §3): the session plays on without this chain.
//
// The main thread sleeps on control_word() (processor.h), not on a
// descriptor, so a watcher thread polls the display's connection and every
// descriptor a plugin registered, and wakes the main thread when one is
// readable. It reads nothing itself: the main thread dispatches, then lets
// it poll again, so a level-triggered descriptor wakes it once per pass.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace sonora::plugin_host {

// One editor's toplevel. The plugin's view is attached into it (VST3
// X11EmbedWindowID); the window itself is ours.
class EditorWindow {
public:
    virtual ~EditorWindow() = default;
    // What the plugin is given as its parent (an X11 Window).
    virtual uint64_t native_handle() const = 0;
    // The plugin's size. A window the plugin cannot resize is held at it
    // (min = max size hints), so a window manager does not stretch it.
    virtual void set_size(uint32_t width, uint32_t height, bool resizable) = 0;
    virtual void size(uint32_t& width, uint32_t& height) const = 0;
    virtual bool resizable() const = 0;
    // Mapped once the plugin is attached, never before: an empty window
    // shown while a slow editor builds itself is a window the operator may
    // close under it.
    virtual void show() = 0;
    virtual void raise() = 0;
    // Set by dispatch(): the operator closed the window (WM_DELETE_WINDOW).
    virtual bool close_requested() const = 0;
    // Set by dispatch(): the operator resized it; taken once.
    virtual bool take_resize(uint32_t& width, uint32_t& height) = 0;
};

// The display connection, the descriptors and timers plugins register (VST3
// Linux::IRunLoop), and the watcher. One per host process, opened at the
// first OpenEditor.
class EditorDisplay {
public:
    using Id = uint64_t;
    virtual ~EditorDisplay() = default;

    // Null, with the reason, where there is no display (DISPLAY unset, or
    // the server refused) or this build has no X11.
    static std::unique_ptr<EditorDisplay> open(std::string& error);

    // An unmapped toplevel titled `title`. `parent_token` is the opaque
    // token of ADR-0007 update 1 item 6: empty on the appliance; otherwise
    // an X11 window id (decimal, or hex after 0x) the editor is transient
    // for. Null, with the reason, on a token that is not one.
    virtual std::unique_ptr<EditorWindow> create_window(const std::string& title, const std::string& parent_token,
                                                        std::string& error) = 0;

    // A descriptor to call `ready` for when readable, and a timer every
    // `interval_ms` (clamped to 1 ms .. 60 s). `owner` groups them, so an
    // editor that forgets to unregister is cleaned up when it closes.
    // Main thread; safe from inside a callback.
    virtual Id watch_fd(int fd, const void* owner, std::function<void()> ready) = 0;
    virtual Id add_timer(uint32_t interval_ms, const void* owner, std::function<void()> fire) = 0;
    virtual void remove(Id id) = 0;
    virtual void remove_fd(const void* owner, int fd) = 0;
    virtual void remove_owner(const void* owner) = 0;
    virtual std::size_t registered(const void* owner) const = 0;

    // Window events, readable descriptors and due timers, then lets the
    // watcher poll again. Returns when the next timer is due (monotonic_ns),
    // or 0 with none: the main loop sleeps no longer than that.
    virtual uint64_t dispatch() = 0;
};

}  // namespace sonora::plugin_host
