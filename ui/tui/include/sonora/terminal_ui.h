// SPDX-License-Identifier: GPL-3.0-or-later
//
// The terminal interface, as a client of the control API (ADR-0007 M4).
//
// It knows the engine only as api::EngineControl and api::StateView: no
// Session, no Track, no backend. Keys resolve through ui/keymap; Interface
// commands (app.quit) run in its own registry, Engine commands go to the
// engine as CommandRequests, and what the engine says comes back through
// poll(). The diagnostic of last resort (ADR-0006), so it says plainly what
// it cannot do: a key that does nothing, an engine that stopped answering.
//
// TerminalView is everything but the terminal itself, so a test drives it
// with chords and reads the frame it draws. run_terminal() adds the tty:
// the key-reading mode, stdin, and the screen.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "sonora/api/engine_control.h"
#include "sonora/command.h"
#include "sonora/keymap.h"

namespace sonora::tui {

class TerminalView {
public:
    // `engine` must be connected.
    TerminalView(api::EngineControl& engine, KeyMap keymap);

    // The interface's own commands; app.quit is installed here.
    CommandRegistry& local() { return local_; }
    bool implemented(const std::string& id) const;

    // Measures the profile against `device` (the chords this terminal can
    // send) and what this build implements, engine and interface together,
    // repairs what it can, and builds the footer. Returns the notices,
    // which also stay on screen. May be called again with another device.
    std::vector<std::string> prepare(const std::vector<KeyChord>& device);
    // Whether the profile as prepared can reach app.quit from `device`.
    bool can_quit(const std::vector<KeyChord>& device) const;
    // A line kept on screen for the session.
    void add_notice(std::string notice) { notices_.push_back(std::move(notice)); }

    void on_chord(const KeyChord& chord);
    void on_unrecognised();

    // One frame: everything the engine sent since the last one, then the
    // status line's countdown. `now_ns` is the monotonic clock the
    // heartbeat is judged on.
    void tick(uint64_t now_ns);
    // The screen, without the escape that clears it. Moves this reader's
    // peak cursors, so call it once per frame.
    std::string render(uint64_t now_ns);

    bool quit() const { return quit_; }
    // The engine's backend stopped (server gone, rate change, device gone).
    bool engine_failed() const { return !fatal_.empty(); }
    const std::string& fatal_reason() const { return fatal_; }
    const std::string& status() const { return status_; }
    const std::vector<std::string>& help() const { return help_; }

private:
    void say(std::string message);
    void run(const std::string& command, const CommandContext& ctx);

    api::EngineControl& engine_;
    KeyMap as_loaded_;
    KeyMap keymap_;
    CommandRegistry local_;
    std::vector<std::string> help_;
    std::vector<std::string> keymap_notices_;
    std::vector<std::string> notices_;
    std::string inputs_missing_;
    std::string take_failed_;
    std::string fatal_;
    std::string status_;
    int status_frames_ = 0;
    bool quit_ = false;
    std::map<uint64_t, std::string> pending_;   // seq -> command, for the status line
    api::Inbox inbox_;
    api::StateFrame frame_;   // kept, so a frame allocates only once
    static constexpr uint64_t kLocatingShownAfterNs = 100'000'000;
    uint64_t locating_since_ns_ = 0;   // 0: no locate held
};

struct TerminalOptions {
    // Checked every frame: a signal asked the process to stop.
    std::function<bool()> stop_requested;
};

enum class TerminalExit {
    Quit,           // app.quit, or stop_requested
    EngineFailed,   // see TerminalView::fatal_reason()
};

// The loop on stdin and stdout until the operator quits or the engine's
// backend fails. `view` is prepared here, against what this terminal can
// actually deliver, and its notices are printed to stderr once. The
// terminal is restored before it returns.
TerminalExit run_terminal(TerminalView& view, const TerminalOptions& options);

}  // namespace sonora::tui
