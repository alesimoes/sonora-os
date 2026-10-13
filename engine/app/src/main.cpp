// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-engine: the recording core. Phase 1 drives it from a terminal UI so
// the audio path can be proven before any graphical shell exists.

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <termios.h>
#include <unistd.h>

#include "sonora/command.h"
#include "sonora/jack_backend.h"
#include "sonora/keymap.h"
#include "sonora/quit_signals.h"
#include "sonora/session.h"
#include "sonora/session_commands.h"
#include "sonora/tty_mode.h"

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
        apply(sonora::key_reading_mode(saved));
    }
    // True if Ctrl+C now raises SIGINT, as read back from the terminal.
    bool keep_interrupt_key() {
        if (!is_tty) return false;
        apply(sonora::interrupt_key_mode(saved));
        return (current.c_lflag & ISIG) && current.c_cc[VINTR] == 0x03;
    }
    void apply(const termios& t) {
        if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0) active = true;
        // tcsetattr reports success if it applied any part of the change, so
        // what the keyboard can reach is read back rather than assumed.
        tcgetattr(STDIN_FILENO, &current);
    }
    bool delivers(unsigned char byte) const {
        return !is_tty || sonora::tty_delivers_byte(current, byte);
    }
    ~TermiosGuard() {
        if (active) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    }
};

// Everything after the last '.' in a command id. Short enough for a footer,
// and unambiguous in practice: "toggle_record", "arm_all", "quit".
std::string short_name(const std::string& command) {
    const std::size_t dot = command.rfind('.');
    return dot == std::string::npos ? command : command.substr(dot + 1);
}

// Builds the footer out of the loaded key map, so what it says is what the
// keyboard does. The literal it replaces read "space=rec/stop 1-8=arm
// a=arm all q=quit" under every profile; under protools.json all four of
// those were wrong. Only chords this terminal can send and commands this
// build implements are listed -- a footer advertising a key that reports
// "not implemented" is the same lie in a politer font.
std::vector<std::string> build_help(const sonora::KeyMap& map,
                                    const sonora::CommandRegistry& reg,
                                    const std::vector<sonora::KeyChord>& device) {
    std::vector<std::string> entries;
    for (const auto& b : map.bindings()) {
        if (!reg.implemented(b.command) || sonora::needs_selection(b)) continue;
        bool sendable = false;
        for (const auto& c : device) {
            if (c == b.chord) {
                sendable = true;
                break;
            }
        }
        if (!sendable) continue;
        std::string e = sonora::KeyMap::chord_to_string(b.chord) + "=" +
                        short_name(b.command);
        if (b.ctx.has_target()) e += std::to_string(b.ctx.target + 1);
        entries.push_back(std::move(e));
    }

    std::vector<std::string> lines;
    if (entries.empty()) {
        // Reachable only if ensure_essentials() could not repair the profile,
        // in which case its notice is already on screen saying so.
        lines.push_back("no key in this profile does anything this build"
                        " implements");
        return lines;
    }
    std::string line;
    for (const std::string& e : entries) {
        if (!line.empty() && line.size() + e.size() + 2 > 74) {
            lines.push_back(line);
            line.clear();
        }
        if (!line.empty()) line += "  ";
        line += e;
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

void draw(sonora::Session& s, sonora::JackBackend& jack, int take,
          const std::string& status, const std::vector<std::string>& help,
          const std::vector<std::string>& notices) {
    using sonora::TransportState;
    const auto st = s.transport().state();
    const char* label = st == TransportState::Recording ? "REC "
                      : st == TransportState::Rolling   ? "PLAY"
                                                        : "STOP";
    const double t = s.transport().seconds();

    std::printf("\033[H\033[J");
    std::printf("  SONORA  %s  %02d:%02d:%06.3f   take %d\n",
                label, static_cast<int>(t) / 3600, (static_cast<int>(t) / 60) % 60,
                t - 60.0 * (static_cast<int>(t) / 60), take);
    std::printf("  %u Hz / %u frames   xruns %u   overruns %u\n\n",
                jack.sample_rate(), jack.buffer_size(), jack.xruns(),
                s.disk().total_overruns());

    for (const auto& tr : s.tracks()) {
        const float peak = tr->take_peak();
        const int bars = static_cast<int>(peak * 40.0f);
        std::printf("  %-10s %s %s  ", tr->name().c_str(),
                    tr->rec_armed() ? "\033[31m[R]\033[0m" : "[ ]",
                    tr->muted() ? "[M]" : "[ ]");
        for (int i = 0; i < 40; ++i) std::putchar(i < bars ? '=' : '.');
        std::printf(" %5.1f dB\n",
                    peak > 0.0f ? 20.0 * std::log10(static_cast<double>(peak))
                                : -99.9);
    }
    std::putchar('\n');
    for (const std::string& l : help) std::printf("  %s\n", l.c_str());
    // Redrawn every frame on purpose: these say the profile cannot do
    // something it claims to, and the screen clear at the top of this function
    // would otherwise erase the start-up message after one frame.
    for (const std::string& n : notices) {
        std::printf("  \033[33m!\033[0m %s\n", n.c_str());
    }
    // A key that did nothing is the failure keymaps/README.md warns about, so
    // say what happened rather than leaving the operator guessing.
    if (!status.empty()) std::printf("  %s\n", status.c_str());
    std::fflush(stdout);
}

// Waits for something to be patched into our inputs. This is not a test
// accommodation: on the appliance the engine starts at boot, and a USB
// interface may not be enumerated yet. Starting a take against ports that
// are about to be connected would record a silent head.
int wait_for_inputs(sonora::JackBackend& jack, double seconds) {
    const int immediate = jack.connected_input_count();
    if (immediate > 0 || seconds <= 0.0) return immediate;

    std::printf("sonora: waiting up to %.0fs for inputs...\n", seconds);
    std::fflush(stdout);

    double waited = 0.0;
    while (waited < seconds && !sonora::quit_requested()) {
        ::usleep(100000);
        waited += 0.1;
        const int n = jack.connected_input_count();
        if (n > 0) {
            // Give the rest of the ports a moment to land, so a 16-channel
            // interface does not start a take with two channels connected.
            ::usleep(500000);
            return jack.connected_input_count();
        }
    }
    return 0;
}

// Records for a fixed duration and reports the result in one machine-readable
// line. Return code is the test verdict: anything non-zero means the recording
// is not trustworthy, even if files were produced.
int run_headless(sonora::Session& session, sonora::JackBackend& jack,
                 double seconds, double wait_seconds) {
    const int connected = wait_for_inputs(jack, wait_seconds);
    if (connected == 0) {
        std::fprintf(stderr,
                     "sonora: refusing to record with no connected inputs;"
                     " the result would be silence.\n");
        return 1;
    }

    // Never take 1 unconditionally: a test run against a session folder that
    // already holds takes must add one, not replace one.
    std::string why;
    if (!session.start_recording(session.next_free_take(), &why)) {
        std::fprintf(stderr, "sonora: cannot record: %s\n", why.c_str());
        return 1;
    }

    const uint64_t target =
        static_cast<uint64_t>(seconds * session.transport().sample_rate());

    // Wall-clock bound as well as a sample bound: if the server stops calling
    // the callback the playhead stops advancing, and waiting on samples alone
    // would hang forever instead of failing.
    const double deadline = seconds * 2.0 + 5.0;
    double waited = 0.0;

    while (!sonora::quit_requested() &&
           session.transport().playhead() < target) {
        if (jack.server_gone()) {
            std::fprintf(stderr, "sonora: the audio server went away.\n");
            session.stop();
            return 1;
        }
        ::usleep(20000);
        waited += 0.02;
        if (waited > deadline) {
            std::fprintf(stderr,
                         "sonora: timed out after %.1fs with %llu of %llu"
                         " frames captured.\n",
                         waited,
                         static_cast<unsigned long long>(
                             session.transport().playhead()),
                         static_cast<unsigned long long>(target));
            session.stop();
            return 1;
        }
    }

    // stop() has already said why on stderr.
    if (!session.stop()) return 1;

    // Read the counters after stop(), so the disk thread has flushed and the
    // overrun total is final rather than a snapshot mid-drain.
    const uint32_t xruns = jack.xruns();
    const uint32_t overruns = session.disk().total_overruns();

    uint64_t frames_on_disk = 0;
    int files = 0;
    for (const auto& tr : session.tracks()) {
        if (tr->writer().frames_written() == 0) continue;
        ++files;
        if (frames_on_disk == 0) {
            frames_on_disk = tr->writer().frames_written();
        } else if (tr->writer().frames_written() != frames_on_disk) {
            // Tracks drifting apart means the capture is unusable even though
            // every individual file is well-formed.
            std::fprintf(stderr,
                         "sonora: tracks disagree on length (%llu vs %llu).\n",
                         static_cast<unsigned long long>(frames_on_disk),
                         static_cast<unsigned long long>(
                             tr->writer().frames_written()));
            return 1;
        }
    }

    std::printf("RESULT files=%d connected=%d frames=%llu requested=%llu"
                " rate=%u buffer=%u xruns=%u overruns=%u\n",
                files, connected,
                static_cast<unsigned long long>(frames_on_disk),
                static_cast<unsigned long long>(target),
                jack.sample_rate(), jack.buffer_size(), xruns, overruns);

    if (files == 0) {
        std::fprintf(stderr, "sonora: no audio was written.\n");
        return 1;
    }
    if (xruns > 0 || overruns > 0) {
        std::fprintf(stderr,
                     "sonora: %u xruns and %u overruns; the recording has"
                     " holes in it.\n", xruns, overruns);
        return 1;
    }
    if (frames_on_disk < target) {
        std::fprintf(stderr,
                     "sonora: short by %llu frames.\n",
                     static_cast<unsigned long long>(target - frames_on_disk));
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    sonora::SessionConfig cfg;
    cfg.dir = std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") +
              "/Sessions/Untitled";

    // Headless operation exists so the engine can be tested without a human
    // at a keyboard. Booting a machine to find out whether a change records
    // is too slow to iterate on.
    double record_seconds = 0.0;  // 0 means interactive
    double wait_for_inputs_seconds = 0.0;
    bool arm_all = false;
    std::string keymap_path;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            cfg.dir = argv[++i];
        } else if (std::strcmp(argv[i], "--tracks") == 0 && i + 1 < argc) {
            ++i;
            if (std::strcmp(argv[i], "auto") == 0) {
                cfg.track_count = 0;  // resolved below, once we can ask
            } else {
                cfg.track_count = std::atoi(argv[i]);
            }
        } else if (std::strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            cfg.sample_rate = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--float") == 0) {
            cfg.format = sonora::WavWriter::Format::Float32;
        } else if (std::strcmp(argv[i], "--record") == 0 && i + 1 < argc) {
            record_seconds = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--wait-for-inputs") == 0 &&
                   i + 1 < argc) {
            wait_for_inputs_seconds = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--arm-all") == 0) {
            arm_all = true;
        } else if (std::strcmp(argv[i], "--keymap") == 0 && i + 1 < argc) {
            keymap_path = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf(
                "usage: sonora-engine [options]\n"
                "  --dir PATH        session directory\n"
                "  --tracks N|auto   track count, or one per capture channel\n"
                "  --rate HZ         session sample rate (default 48000)\n"
                "  --float           record 32-bit float instead of 24-bit\n"
                "  --arm-all         arm every track at startup\n"
                "  --keymap PATH     key map to use instead of the built-in one\n"
                "  --record SECONDS  record for SECONDS, then exit (headless)\n"
                "  --wait-for-inputs SECONDS\n"
                "                    wait for a capture connection before recording\n");
            return 0;
        } else {
            std::fprintf(stderr, "sonora: unknown option %s\n", argv[i]);
            return 2;
        }
    }

    if (record_seconds > 0.0 && !arm_all) {
        // Recording nothing for ten seconds and exiting 0 is a test that
        // passes while proving nothing. Refuse it.
        std::fprintf(stderr,
                     "sonora: --record needs at least one armed track;"
                     " pass --arm-all.\n");
        return 2;
    }

    // Before anything that waits: wait_for_inputs() can sit for a minute at
    // boot, and a SIGTERM or hangup in that time must still end in a clean
    // stop. The keyboard raises none of these -- the key reading mode turns
    // ISIG off, because Ctrl+C is edit.copy in a profile -- unless the
    // profile cannot quit; see keep_interrupt_key().
    sonora::install_quit_signals();

    // Loaded before anything touches hardware: a bad key map is a startup
    // error, not something to discover halfway into a session.
    sonora::KeyMap keymap = sonora::KeyMap::built_in_default();
    if (!keymap_path.empty()) {
        std::string err;
        if (!sonora::KeyMap::load_file(keymap_path, keymap, err)) {
            std::fprintf(stderr, "sonora: %s\n", err.c_str());
            return 2;
        }
        std::printf("sonora: key map \"%s\", %zu bindings\n",
                    keymap.name().c_str(), keymap.bindings().size());
    }

    if (cfg.track_count == 0) {
        // One track per capture channel the interface offers. Guessing a
        // number means either silent tracks or inputs nobody is recording.
        const int channels = sonora::probe_capture_channels("sonora-probe");
        if (channels == 0) {
            std::fprintf(stderr,
                         "sonora: --tracks auto found no capture channels."
                         " Is the interface connected?\n");
            return 1;
        }
        cfg.track_count = channels;
        std::printf("sonora: %d capture channels detected\n", channels);
    }

    sonora::Session session(cfg);
    if (!session.create_dirs()) {
        std::fprintf(stderr, "sonora: cannot create session at %s\n",
                     cfg.dir.c_str());
        return 1;
    }

    sonora::JackBackend jack;
    if (!jack.open("sonora", &session)) {
        std::fprintf(stderr, "sonora: %s\n", jack.error().c_str());
        return 1;
    }
    // The server decides the rate; a mismatch would write wrong-pitch files.
    if (jack.sample_rate() != cfg.sample_rate) {
        std::fprintf(stderr,
                     "sonora: server runs at %u Hz, session asked for %u Hz\n",
                     jack.sample_rate(), cfg.sample_rate);
        return 1;
    }
    if (!jack.activate()) {
        std::fprintf(stderr, "sonora: %s\n", jack.error().c_str());
        return 1;
    }
    int auto_connected = jack.auto_connect_inputs();
    if (auto_connected == 0 && wait_for_inputs_seconds > 0.0) {
        // Retry the auto-connect once ports appear, since the interface may
        // simply not have been enumerated when we started.
        if (wait_for_inputs(jack, wait_for_inputs_seconds) == 0) {
            auto_connected = jack.auto_connect_inputs();
        }
    }
    if (auto_connected == 0 && jack.connected_input_count() == 0) {
        // Every track would record silence. On real hardware this means the
        // interface was not detected; in a test harness it means nothing was
        // patched in. Either way, recording nothing while saying nothing is
        // the one outcome the operator must never get.
        std::fprintf(stderr,
                     "sonora: no capture ports were connected. Recording"
                     " now would produce silence.\n");
    }
    session.disk().start();


    if (arm_all) {
        for (const auto& tr : session.tracks()) tr->set_rec_armed(true);
    }

    if (record_seconds > 0.0) {
        const int rc = run_headless(session, jack, record_seconds,
                                    wait_for_inputs_seconds);
        jack.close();
        return rc;
    }

    // From the folder, not from 1: systemd restarts the engine every time it
    // exits, and a counter that started over numbered the next take like the
    // last session's first one.
    int take = session.next_free_take();
    bool server_lost = false;
    bool quit = false;
    std::string status;

    sonora::CommandRegistry commands;
    sonora::install_session_commands(commands, session, take, quit);

    // The terminal mode is set before anything is measured against it: which
    // control bytes reach us depends on it, and a check made against the
    // cooked mode would believe Ctrl+Q quits when flow control eats it.
    TermiosGuard term;
    term.raw();

    // Checked against the terminal, not against a keyboard in the abstract:
    // "cmd+q" and "ctrl+shift+z" are real bindings that this input device
    // cannot send, and a profile whose only way to arm a track is one of those
    // cannot record however correct the file is.
    const auto implemented = [&commands](const std::string& id) {
        return commands.implemented(id);
    };
    const auto terminal_device = [&term]() {
        return sonora::KeyMap::terminal_chords(
            [&term](unsigned char byte) { return term.delivers(byte); });
    };
    std::vector<sonora::KeyChord> device = terminal_device();
    const sonora::KeyMap as_loaded = keymap;
    std::vector<std::string> notices = keymap.ensure_essentials(device, implemented);

    // With signals off and no key that runs app.quit, nothing on the keyboard
    // could leave the engine. Ctrl+C is then given back to the terminal, and
    // the profile is measured again against what the keyboard can still send
    // -- Ctrl+C is no longer one of those keys, and the footer must not say
    // it is.
    const std::vector<std::string>* quit_group = nullptr;
    for (const auto& g : sonora::KeyMap::essential_commands()) {
        for (const auto& id : g) {
            if (id == "app.quit") quit_group = &g;
        }
    }
    if (quit_group != nullptr &&
        !keymap.reaches(*quit_group, device, implemented) && term.is_tty) {
        const bool interrupt = term.keep_interrupt_key();
        device = terminal_device();
        keymap = as_loaded;
        notices = keymap.ensure_essentials(device, implemented);
        if (interrupt) {
            notices.push_back("Ctrl+C leaves the engine: no key in this profile"
                              " can, so it is not passed to the profile");
        }
    }
    for (const std::string& n : notices) {
        std::fprintf(stderr, "sonora: key map \"%s\": %s\n",
                     keymap.name().c_str(), n.c_str());
    }
    const std::vector<std::string> help = build_help(keymap, commands, device);

    sonora::TerminalInput input;
    // A message stays up for about two seconds. Clearing it only on the next
    // successful dispatch left "not implemented in this build" on screen
    // indefinitely, which stops being information and becomes furniture.
    int status_frames = 0;
    const auto say = [&status, &status_frames](std::string message) {
        status = std::move(message);
        status_frames = 40;
    };

    while (!sonora::quit_requested() && !quit) {
        // The server dying stops the callback without stopping us. Close
        // the take so what was captured is a valid file, then let the
        // service unit restart us against a fresh server.
        if (jack.server_gone()) {
            server_lost = true;
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
            if (e.result == sonora::TerminalInput::Result::Unrecognised) {
                say("that key means nothing here");
                continue;
            }
            if (const sonora::Binding* b = keymap.resolve(e.chord)) {
                std::string why;
                const sonora::DispatchResult d =
                    commands.dispatch(b->command, b->ctx, why);
                if (d == sonora::DispatchResult::Ok) {
                    status.clear();
                    status_frames = 0;
                } else {
                    say(b->command + ": " +
                        (why.empty() ? sonora::dispatch_result_text(d) : why));
                }
            } else {
                say(sonora::KeyMap::chord_to_string(e.chord) + " is not bound");
            }
        }

        draw(session, jack, take, status, help, notices);
        if (status_frames > 0 && --status_frames == 0) status.clear();
        ::usleep(50000);  // 20 fps is plenty for meters
    }

    session.stop();
    jack.close();
    std::printf("\nsonora: session written to %s\n", cfg.dir.c_str());

    if (server_lost) {
        std::fprintf(stderr, "sonora: the audio server went away.\n");
        return 1;
    }
    return 0;
}
