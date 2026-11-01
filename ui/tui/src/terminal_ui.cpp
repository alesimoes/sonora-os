// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/terminal_ui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <utility>

#include <termios.h>
#include <unistd.h>

#include "sonora/tty_mode.h"

namespace sonora::tui {

namespace {

// Everything after the last '.' in a command id. Short enough for a footer,
// and unambiguous in practice: "toggle_record", "arm_all", "quit".
std::string short_name(const std::string& command) {
    const std::size_t dot = command.rfind('.');
    return dot == std::string::npos ? command : command.substr(dot + 1);
}

// A message stays up for about two seconds at the 50 ms frame. Clearing it
// only on the next successful dispatch left "not implemented in this build"
// on screen indefinitely, which stops being information and becomes
// furniture.
constexpr int kStatusFrames = 40;

template <typename... Args>
void append(std::string& out, const char* fmt, Args... args) {
    char buf[512];
    const int n = std::snprintf(buf, sizeof buf, fmt, args...);
    if (n > 0) out.append(buf, static_cast<std::size_t>(std::min<int>(n, sizeof buf - 1)));
}

uint64_t steady_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

}  // namespace

TerminalView::TerminalView(api::EngineControl& engine, KeyMap keymap)
    : engine_(engine), as_loaded_(keymap), keymap_(std::move(keymap)) {
    local_.install("app.quit", [this](const CommandContext&) {
        quit_ = true;
        return true;
    });
}

bool TerminalView::implemented(const std::string& id) const {
    if (local_.implemented(id)) return true;
    const auto& engine = engine_.hello().implemented_commands;
    return std::find(engine.begin(), engine.end(), id) != engine.end();
}

bool TerminalView::can_quit(const std::vector<KeyChord>& device) const {
    for (const auto& g : KeyMap::essential_commands()) {
        for (const auto& id : g) {
            if (id == "app.quit") {
                return keymap_.reaches(g, device,
                                       [this](const std::string& c) { return implemented(c); });
            }
        }
    }
    return true;
}

std::vector<std::string> TerminalView::prepare(const std::vector<KeyChord>& device) {
    keymap_ = as_loaded_;
    // Checked against the terminal, not against a keyboard in the abstract:
    // "cmd+q" and "ctrl+shift+z" are real bindings that this input device
    // cannot send, and a profile whose only way to arm a track is one of those
    // cannot record however correct the file is.
    keymap_notices_ = keymap_.ensure_essentials(
        device, [this](const std::string& id) { return implemented(id); });

    // The footer out of the loaded key map, so what it says is what the
    // keyboard does. Only chords this terminal can send and commands this
    // build implements are listed -- a footer advertising a key that reports
    // "not implemented" is the same lie in a politer font.
    std::vector<std::string> entries;
    for (const auto& b : keymap_.bindings()) {
        if (!implemented(b.command) || needs_selection(b)) continue;
        if (std::find(device.begin(), device.end(), b.chord) == device.end()) continue;
        std::string e = KeyMap::chord_to_string(b.chord) + "=" + short_name(b.command);
        if (b.ctx.has_target()) e += std::to_string(b.ctx.target + 1);
        entries.push_back(std::move(e));
    }
    help_.clear();
    if (entries.empty()) {
        // Reachable only if ensure_essentials() could not repair the profile,
        // in which case its notice is already on screen saying so.
        help_.push_back("no key in this profile does anything this build implements");
    } else {
        std::string line;
        for (const std::string& e : entries) {
            if (!line.empty() && line.size() + e.size() + 2 > 74) {
                help_.push_back(line);
                line.clear();
            }
            if (!line.empty()) line += "  ";
            line += e;
        }
        if (!line.empty()) help_.push_back(line);
    }
    return keymap_notices_;
}

void TerminalView::say(std::string message) {
    status_ = std::move(message);
    status_frames_ = kStatusFrames;
}

void TerminalView::run(const std::string& command, const CommandContext& ctx) {
    // The interface's own first: app.quit closes this, not the engine's
    // session (ADR-0007 §5).
    if (local_.implemented(command)) {
        std::string why;
        const DispatchResult d = local_.dispatch(command, ctx, why);
        if (d == DispatchResult::Ok) {
            status_.clear();
            status_frames_ = 0;
        } else {
            say(command + ": " + (why.empty() ? dispatch_result_text(d) : why));
        }
        return;
    }
    if (!implemented(command)) {
        say(command + ": " + dispatch_result_text(DispatchResult::NotImplemented));
        return;
    }
    api::CommandRequest r;
    r.command = command;
    r.target = ctx.target;
    r.argument = ctx.argument;
    pending_[engine_.send(r)] = command;
}

void TerminalView::on_chord(const KeyChord& chord) {
    if (const Binding* b = keymap_.resolve(chord)) {
        run(b->command, b->ctx);
    } else {
        say(KeyMap::chord_to_string(chord) + " is not bound");
    }
}

void TerminalView::on_unrecognised() { say("that key means nothing here"); }

void TerminalView::tick(uint64_t /*now_ns*/) {
    inbox_.clear();
    engine_.poll(inbox_);
    for (const api::CommandReply& r : inbox_.replies) {
        const auto it = pending_.find(r.seq);
        const std::string command = it != pending_.end() ? it->second : "command";
        if (it != pending_.end()) pending_.erase(it);
        // A key that did nothing is the failure keymaps/README.md warns
        // about, so say what happened rather than leaving the operator
        // guessing.
        if (r.result == DispatchResult::Ok) {
            status_.clear();
            status_frames_ = 0;
        } else {
            say(command + ": " + (r.message.empty() ? dispatch_result_text(r.result) : r.message));
        }
    }
    for (const api::EngineEvent& e : inbox_.events) {
        switch (e.kind) {
            case api::EventKind::Notice:
                notices_.push_back(e.text);
                break;
            case api::EventKind::PluginFault:
                // Empty: the fault cleared, which a notice line cannot show.
                if (!e.text.empty()) notices_.push_back(e.text);
                break;
            case api::EventKind::InputsMissing:
                inputs_missing_ = e.text;
                break;
            case api::EventKind::TakeFailed:
                take_failed_ = e.text;
                break;
            case api::EventKind::Fatal:
                fatal_ = e.text.empty() ? "the audio backend failed" : e.text;
                break;
            case api::EventKind::RecoveryOffer:
                // Placed from the editor, which shows where each take goes;
                // here the operator is only told they are waiting.
                // Said again at every open while they stay unplaced: once.
                if (!e.text.empty()) {
                    std::string line = std::to_string(e.frame) +
                                       " recovered take file(s) are not on the timeline;"
                                       " the editor offers to place them";
                    if (std::find(notices_.begin(), notices_.end(), line) == notices_.end()) {
                        notices_.push_back(std::move(line));
                    }
                }
                break;
            case api::EventKind::SessionFolder:
                // This interface saves nothing, so it has no use for where.
                break;
            case api::EventKind::Latency:
                // Measured and set from the Qt interface's Latency window or
                // --measure-latency; the engine says L at start on stderr.
                break;
        }
    }
    // Model updates are for an editor's replica; this interface draws none.
    if (status_frames_ > 0 && --status_frames_ == 0) status_.clear();
}

std::string TerminalView::render(uint64_t now_ns) {
    api::StateView& v = engine_.state();
    const api::Hello& h = engine_.hello();
    const bool alive = v.engine_alive(now_ns);
    v.frame(frame_);
    const api::TransportSnapshot& t = frame_.transport;
    std::string out;

    const char* label = t.state == api::TransportState::Recording ? "REC "
                        : t.state == api::TransportState::Rolling ? "PLAY"
                                                                  : "STOP";
    const double secs = h.sample_rate > 0 ? static_cast<double>(t.playhead) / h.sample_rate : 0.0;
    const int whole = static_cast<int>(secs);
    // ADR-0009 §4.4: only a locate held back past 100 ms is said, so the
    // word does not flash on every click of the ruler.
    const bool held = !t.stale && (t.transport_flags & api::kTransportLocating) != 0;
    if (!held) {
        locating_since_ns_ = 0;
    } else if (locating_since_ns_ == 0) {
        locating_since_ns_ = now_ns;
    }
    const bool locating = held && now_ns >= locating_since_ns_ &&
                          now_ns - locating_since_ns_ >= kLocatingShownAfterNs;
    append(out, "  SONORA  %s  %02d:%02d:%06.3f   take %d%s%s\n", label, whole / 3600,
           (whole / 60) % 60, secs - 60.0 * (whole / 60), t.take,
           locating ? "   Locating\xE2\x80\xA6" : "", t.stale ? "   (stale)" : "");
    // Each miss is an audible hole in playback: red once there is one.
    append(out, "  %u Hz / %u frames   xruns %u   clock steps %u   overruns %u   %smisses %u%s\n\n",
           h.sample_rate, t.buffer_frames, t.xruns, t.clock_steps, t.overruns,
           t.playback_misses > 0 ? "\033[31m" : "", t.playback_misses,
           t.playback_misses > 0 ? "\033[0m" : "");

    const auto meter = [&out, alive](float peak) {
        if (!alive) {
            // A control that lies is worse than one that is missing
            // (ADR-0007 §3): no frozen bar from before the engine stopped.
            out += "  no data\n";
            return;
        }
        const int bars = static_cast<int>(peak * 40.0f);
        for (int b = 0; b < 40; ++b) out.push_back(b < bars ? '=' : '.');
        append(out, " %5.1f dB\n",
               peak > 0.0f ? 20.0 * std::log10(static_cast<double>(peak)) : -99.9);
    };
    for (uint32_t i = 0; i < h.track_count; ++i) {
        api::TrackSnapshot s;
        if (i < frame_.tracks.size()) s = frame_.tracks[i];
        const float peak = v.take_peak(i);
        append(out, "  %-10s %s %s  ", h.track_names[i].c_str(),
               s.armed() ? "\033[31m[R]\033[0m" : "[ ]", s.muted() ? "[M]" : "[ ]");
        meter(peak);
    }
    for (uint32_t side = 0; side < api::kBusPeakSides; ++side) {
        const float peak = v.take_bus_peak(side);
        append(out, "  %-10s          ", side == 0 ? "Bus L" : "Bus R");
        meter(peak);
    }
    out.push_back('\n');
    for (const std::string& l : help_) append(out, "  %s\n", l.c_str());
    // Redrawn every frame on purpose: these say the profile cannot do
    // something it claims to, and the screen clear at the top of the frame
    // would otherwise erase the start-up message after one frame.
    for (const std::string& n : keymap_notices_) append(out, "  \033[33m!\033[0m %s\n", n.c_str());
    for (const std::string& n : notices_) append(out, "  \033[33m!\033[0m %s\n", n.c_str());
    if (!alive) {
        out += "  \033[31m! the engine is not answering: meters show no data\033[0m\n";
    }
    if (!inputs_missing_.empty()) {
        append(out, "  \033[31m!\033[0m no inputs: %s\n", inputs_missing_.c_str());
    }
    if (!take_failed_.empty()) append(out, "  \033[31m! %s\033[0m\n", take_failed_.c_str());
    if (!fatal_.empty()) append(out, "  \033[31m! %s\033[0m\n", fatal_.c_str());
    if (!status_.empty()) append(out, "  %s\n", status_.c_str());
    return out;
}

namespace {

struct TermiosGuard {
    termios saved{};
    termios current{};
    bool is_tty = false;
    bool active = false;

    void raw() {
        // Not a terminal: bytes arrive exactly as written, with no line
        // discipline to eat any of them.
        if (tcgetattr(STDIN_FILENO, &saved) != 0) return;
        is_tty = true;
        current = saved;
        apply(key_reading_mode(saved));
    }
    // True if Ctrl+C now raises SIGINT, as read back from the terminal.
    bool keep_interrupt_key() {
        if (!is_tty) return false;
        apply(interrupt_key_mode(saved));
        return (current.c_lflag & ISIG) && current.c_cc[VINTR] == 0x03;
    }
    void apply(const termios& t) {
        if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0) active = true;
        // tcsetattr reports success if it applied any part of the change, so
        // what the keyboard can reach is read back rather than assumed.
        tcgetattr(STDIN_FILENO, &current);
    }
    bool delivers(unsigned char byte) const {
        return !is_tty || tty_delivers_byte(current, byte);
    }
    // Before anything is printed on the way out: in the key reading mode a
    // newline does not return the carriage, and the last words staircase.
    void restore() {
        if (active) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        active = false;
    }
    ~TermiosGuard() { restore(); }
};

}  // namespace

TerminalExit run_terminal(TerminalView& view, const TerminalOptions& options) {
    // The terminal mode is set before anything is measured against it:
    // which control bytes reach us depends on it, and a check made against
    // the cooked mode would believe Ctrl+Q quits when flow control eats it.
    TermiosGuard term;
    term.raw();
    const auto terminal_device = [&term]() {
        return KeyMap::terminal_chords([&term](unsigned char byte) { return term.delivers(byte); });
    };
    std::vector<KeyChord> device = terminal_device();
    std::vector<std::string> notices = view.prepare(device);

    // With signals off and no key that runs app.quit, nothing on the keyboard
    // could leave. Ctrl+C is then given back to the terminal, and the profile
    // is measured again against what the keyboard can still send -- Ctrl+C
    // is no longer one of those keys, and the footer must not say it is.
    if (!view.can_quit(device) && term.is_tty) {
        const bool interrupt = term.keep_interrupt_key();
        device = terminal_device();
        notices = view.prepare(device);
        if (interrupt) {
            const std::string n =
                "Ctrl+C leaves the engine: no key in this profile can, so it is not passed to"
                " the profile";
            notices.push_back(n);
            view.add_notice(n);
        }
    }
    for (const std::string& n : notices) std::fprintf(stderr, "sonora: key map: %s\n", n.c_str());

    TerminalInput input;
    TerminalExit exit = TerminalExit::Quit;
    for (;;) {
        if ((options.stop_requested && options.stop_requested()) || view.quit()) break;
        // The engine's backend died, or changed rate or buffer size: the
        // engine closes the take; this interface leaves so the service unit
        // can restart both against a fresh server.
        if (view.engine_failed()) {
            exit = TerminalExit::EngineFailed;
            break;
        }

        // Read whatever is waiting, not one byte: an arrow key is three
        // bytes, F5 is five, and a reader that takes one at a time dispatched
        // the tail of every sequence as individual key presses. A frame in
        // which nothing arrives is what lets a lone Escape be the Escape key;
        // see TerminalInput::frame.
        char buf[64];
        const ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
        for (const auto& e : input.frame(buf, static_cast<long>(n))) {
            if (e.result == TerminalInput::Result::Unrecognised) {
                view.on_unrecognised();
            } else {
                view.on_chord(e.chord);
            }
        }

        const uint64_t now = steady_ns();
        view.tick(now);
        const std::string frame = view.render(now);
        std::printf("\033[H\033[J%s", frame.c_str());
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));  // 20 fps is plenty
    }

    term.restore();
    std::printf("\n");
    std::fflush(stdout);
    return exit;
}

}  // namespace sonora::tui
