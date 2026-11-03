// Virtual keyboard and pointer for tools/test-labwc-input.sh.
//
// wtype and wlrctl cannot do what that test needs. wtype uploads a keymap
// of its own with made-up keycodes, so a nested compositor (which reads raw
// evdev keycodes through its own XKB keymap) sees the wrong keys; wlrctl has
// no separate button press and release, so it cannot drag. This client sends
// real evdev keycodes with an evdev keymap, and pointer buttons and absolute
// motion one event at a time.
//
// The two protocols are declared by hand from their published XML
// (virtual-keyboard-unstable-v1, wlr-virtual-pointer-unstable-v1): Debian
// ships neither XML in a -dev package, and the requests used are few.
//
// Commands on stdin, one per line:
//   key CODE 1|0     evdev key press or release (linux/input-event-codes.h)
//   abs X Y          pointer to layout pixel X,Y (extent from argv)
//   btn CODE 1|0     pointer button press or release (BTN_LEFT is 272)
//   wait MS          sleep
// Usage: labwc-vinput WIDTH HEIGHT < commands
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

static const struct wl_interface vkbd_interface;
static const struct wl_interface vptr_interface;

static const struct wl_interface *vkbd_create_types[] = {&wl_seat_interface, &vkbd_interface};
static const struct wl_message vkbd_manager_requests[] = {
    {"create_virtual_keyboard", "on", vkbd_create_types},
};
static const struct wl_interface vkbd_manager_interface = {
    "zwp_virtual_keyboard_manager_v1", 1, 1, vkbd_manager_requests, 0, NULL};

static const struct wl_interface *no_types[] = {NULL, NULL, NULL, NULL, NULL};
static const struct wl_message vkbd_requests[] = {
    {"keymap", "uhu", no_types},
    {"key", "uuu", no_types},
    {"modifiers", "uuuu", no_types},
    {"destroy", "", no_types},
};
static const struct wl_interface vkbd_interface = {
    "zwp_virtual_keyboard_v1", 1, 4, vkbd_requests, 0, NULL};

static const struct wl_interface *vptr_create_types[] = {&wl_seat_interface, &vptr_interface};
static const struct wl_message vptr_manager_requests[] = {
    {"create_virtual_pointer", "?on", vptr_create_types},
    {"destroy", "", no_types},
};
static const struct wl_interface vptr_manager_interface = {
    "zwlr_virtual_pointer_manager_v1", 1, 2, vptr_manager_requests, 0, NULL};

// Opcodes are positions in this table; it must follow the XML's order.
enum { VPTR_MOTION_ABSOLUTE = 1, VPTR_BUTTON = 2, VPTR_FRAME = 4 };
static const struct wl_message vptr_requests[] = {
    {"motion", "uff", no_types},
    {"motion_absolute", "uuuuu", no_types},
    {"button", "uuu", no_types},
    {"axis", "uuf", no_types},
    {"frame", "", no_types},
    {"axis_source", "u", no_types},
    {"axis_stop", "uu", no_types},
    {"axis_discrete", "uufi", no_types},
    {"destroy", "", no_types},
};
static const struct wl_interface vptr_interface = {
    "zwlr_virtual_pointer_v1", 1, 9, vptr_requests, 0, NULL};

enum { VKBD_KEYMAP = 0, VKBD_KEY = 1, VKBD_MODIFIERS = 2 };

static struct wl_seat *seat;
static struct wl_proxy *vkbd_manager;
static struct wl_proxy *vptr_manager;

static void global(void *data, struct wl_registry *registry, uint32_t name,
                   const char *interface, uint32_t version) {
    (void)data;
    (void)version;
    if (strcmp(interface, "wl_seat") == 0 && !seat)
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    else if (strcmp(interface, vkbd_manager_interface.name) == 0)
        vkbd_manager = wl_registry_bind(registry, name, &vkbd_manager_interface, 1);
    else if (strcmp(interface, vptr_manager_interface.name) == 0)
        vptr_manager = wl_registry_bind(registry, name, &vptr_manager_interface, 1);
}
static void global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}
static const struct wl_registry_listener registry_listener = {global, global_remove};

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: labwc-vinput WIDTH HEIGHT < commands\n");
        return 2;
    }
    uint32_t width = (uint32_t)atoi(argv[1]), height = (uint32_t)atoi(argv[2]);

    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "labwc-vinput: no Wayland display\n");
        return 1;
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!seat || !vkbd_manager || !vptr_manager) {
        fprintf(stderr, "labwc-vinput: compositor lacks seat, virtual keyboard or virtual pointer\n");
        return 1;
    }

    struct wl_proxy *vkbd = wl_proxy_marshal_flags(vkbd_manager, 0, &vkbd_interface, 1, 0, seat, NULL);
    struct wl_proxy *vptr = wl_proxy_marshal_flags(vptr_manager, 0, &vptr_interface, 1, 0, seat, NULL);

    // srvrkeys:none on our side: otherwise the compositor we connect to would
    // see XF86Switch_VT_n itself and never pass the chord on to its client.
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_rule_names names = {"evdev", "pc105", "us", "", "srvrkeys:none"};
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap) {
        fprintf(stderr, "labwc-vinput: cannot compile the evdev keymap\n");
        return 1;
    }
    char *text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    size_t size = strlen(text) + 1;
    int fd = memfd_create("keymap", MFD_CLOEXEC);
    if (fd < 0 || write(fd, text, size) != (ssize_t)size) {
        perror("labwc-vinput: keymap");
        return 1;
    }
    wl_proxy_marshal_flags(vkbd, VKBD_KEYMAP, NULL, 1, 0, XKB_KEYMAP_FORMAT_TEXT_V1, fd, (uint32_t)size);
    wl_display_roundtrip(display);
    struct xkb_state *state = xkb_state_new(keymap);

    char line[128];
    while (fgets(line, sizeof line, stdin)) {
        char cmd[16];
        int a = 0, b = 0;
        int n = sscanf(line, "%15s %d %d", cmd, &a, &b);
        if (n < 1)
            continue;
        if (strcmp(cmd, "key") == 0 && n == 3) {
            wl_proxy_marshal_flags(vkbd, VKBD_KEY, NULL, 1, 0, now_ms(), (uint32_t)a, (uint32_t)b);
            // wlroots does not derive modifiers from a virtual keyboard's
            // keys; without this Ctrl and Alt are never held.
            xkb_state_update_key(state, (xkb_keycode_t)a + 8, b ? XKB_KEY_DOWN : XKB_KEY_UP);
            wl_proxy_marshal_flags(vkbd, VKBD_MODIFIERS, NULL, 1, 0,
                                   xkb_state_serialize_mods(state, XKB_STATE_MODS_DEPRESSED),
                                   xkb_state_serialize_mods(state, XKB_STATE_MODS_LATCHED),
                                   xkb_state_serialize_mods(state, XKB_STATE_MODS_LOCKED),
                                   xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_EFFECTIVE));
        } else if (strcmp(cmd, "abs") == 0 && n == 3) {
            wl_proxy_marshal_flags(vptr, VPTR_MOTION_ABSOLUTE, NULL, 1, 0, now_ms(), (uint32_t)a,
                                   (uint32_t)b, width, height);
            wl_proxy_marshal_flags(vptr, VPTR_FRAME, NULL, 1, 0);
        } else if (strcmp(cmd, "btn") == 0 && n == 3) {
            wl_proxy_marshal_flags(vptr, VPTR_BUTTON, NULL, 1, 0, now_ms(), (uint32_t)a, (uint32_t)b);
            wl_proxy_marshal_flags(vptr, VPTR_FRAME, NULL, 1, 0);
        } else if (strcmp(cmd, "wait") == 0 && n == 2) {
            usleep((useconds_t)a * 1000);
        } else {
            fprintf(stderr, "labwc-vinput: bad command: %s", line);
            return 2;
        }
        wl_display_roundtrip(display);
    }
    wl_display_roundtrip(display);
    wl_display_disconnect(display);
    return 0;
}
