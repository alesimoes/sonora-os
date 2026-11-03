// SPDX-License-Identifier: GPL-3.0-or-later
#include "x11_probe.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

namespace x11_probe {
namespace {

// A window that goes between the query and the look is not an error here.
int quiet(Display*, XErrorEvent*) { return 0; }

struct Connection {
    Display* d = XOpenDisplay(nullptr);
    Connection() {
        if (d) XSetErrorHandler(quiet);
    }
    ~Connection() {
        if (d) XCloseDisplay(d);
    }
};

bool pid_of(Display* d, Window w, long& pid) {
    Atom type = 0;
    int format = 0;
    unsigned long n = 0, after = 0;
    unsigned char* data = nullptr;
    const Atom prop = XInternAtom(d, "_NET_WM_PID", False);
    if (XGetWindowProperty(d, w, prop, 0, 1, False, XA_CARDINAL, &type, &format, &n, &after, &data) != Success ||
        !data) {
        return false;
    }
    const bool ok = n == 1 && format == 32;
    if (ok) pid = *reinterpret_cast<long*>(data);
    XFree(data);
    return ok;
}

}  // namespace

int find(pid_t pid, Toplevel& first) {
    Connection c;
    if (!c.d) return -1;
    Window root = 0, parent = 0;
    Window* children = nullptr;
    unsigned int n = 0;
    if (!XQueryTree(c.d, DefaultRootWindow(c.d), &root, &parent, &children, &n)) return -1;
    int found = 0;
    for (unsigned int i = 0; i < n; ++i) {
        long p = 0;
        if (!pid_of(c.d, children[i], p) || p != long(pid)) continue;
        if (found++ == 0) {
            first.id = children[i];
            char* name = nullptr;
            if (XFetchName(c.d, children[i], &name) && name) {
                first.name = name;
                XFree(name);
            }
            XClassHint klass{};
            if (XGetClassHint(c.d, children[i], &klass)) {
                if (klass.res_class) first.res_class = klass.res_class;
                XFree(klass.res_name);
                XFree(klass.res_class);
            }
            XWindowAttributes wa{};
            if (XGetWindowAttributes(c.d, children[i], &wa)) {
                first.width = wa.width;
                first.height = wa.height;
                first.viewable = wa.map_state == IsViewable;
            }
            XSizeHints hints{};
            long supplied = 0;
            if (XGetWMNormalHints(c.d, children[i], &hints, &supplied))
                first.fixed_size = (hints.flags & PMinSize) && (hints.flags & PMaxSize) &&
                                   hints.min_width == hints.max_width && hints.min_height == hints.max_height;
        }
    }
    if (children) XFree(children);
    return found;
}

bool request_close(uint64_t id) {
    Connection c;
    if (!c.d) return false;
    XEvent e{};
    e.xclient.type = ClientMessage;
    e.xclient.window = Window(id);
    e.xclient.message_type = XInternAtom(c.d, "WM_PROTOCOLS", False);
    e.xclient.format = 32;
    e.xclient.data.l[0] = long(XInternAtom(c.d, "WM_DELETE_WINDOW", False));
    e.xclient.data.l[1] = CurrentTime;
    const bool sent = XSendEvent(c.d, Window(id), False, NoEventMask, &e) != 0;
    XSync(c.d, False);
    return sent;
}

}  // namespace x11_probe
