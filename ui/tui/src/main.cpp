// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-tui: the terminal interface as a process of its own (ADR-0007 §3,
// M7), a client on the engine's control socket. sonora-engine starts it on
// its terminal; it can as well be started by hand beside a running engine,
// as a second interface next to the Qt one.
//
// Exit status: 0 the operator quit, 1 no engine answered, 2 bad options or
// key map, 3 the engine's backend failed.
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

#include "sonora/client/socket_engine.h"
#include "sonora/keymap.h"
#include "sonora/terminal_ui.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_stop(int) { g_stop = 1; }

}  // namespace

int main(int argc, char** argv) {
    sonora::client::SocketEngine::Options opts;
    std::string keymap_path;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--control-socket") == 0 && i + 1 < argc) {
            opts.endpoint = argv[++i];
        } else if (std::strcmp(argv[i], "--keymap") == 0 && i + 1 < argc) {
            keymap_path = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf(
                "usage: sonora-tui [options]\n"
                "  --control-socket PATH  the engine's socket (default\n"
                "                         $XDG_RUNTIME_DIR/sonora/engine.sock)\n"
                "  --keymap PATH          key map to use instead of the built-in one\n");
            return 0;
        } else {
            std::fprintf(stderr, "sonora-tui: unknown option %s\n", argv[i]);
            return 2;
        }
    }

    // Before connecting: a bad key map is a startup error, not something to
    // discover halfway into a session.
    sonora::KeyMap keymap = sonora::KeyMap::built_in_default();
    if (!keymap_path.empty()) {
        std::string err;
        if (!sonora::KeyMap::load_file(keymap_path, keymap, err)) {
            std::fprintf(stderr, "sonora: %s\n", err.c_str());
            return 2;
        }
        std::printf("sonora: key map \"%s\", %zu bindings\n", keymap.name().c_str(),
                    keymap.bindings().size());
    }

    // A SIGTERM or hangup restores the terminal and leaves, as a quit key
    // does; the engine and its take are not this process's to stop.
    std::signal(SIGINT, on_stop);
    std::signal(SIGTERM, on_stop);
    std::signal(SIGHUP, on_stop);

    sonora::client::SocketEngine engine(opts);
    std::string error;
    if (!engine.connect(error)) {
        std::fprintf(stderr, "sonora-tui: %s\n", error.c_str());
        return 1;
    }

    sonora::tui::TerminalView view(engine, keymap);
    sonora::tui::TerminalOptions terminal;
    terminal.stop_requested = []() { return g_stop != 0; };
    const sonora::tui::TerminalExit exit = sonora::tui::run_terminal(view, terminal);
    if (exit == sonora::tui::TerminalExit::EngineFailed) {
        std::fprintf(stderr, "sonora: %s.\n", view.fatal_reason().c_str());
        return 3;
    }
    return 0;
}
