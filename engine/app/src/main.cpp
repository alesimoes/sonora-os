// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-engine: the recording core. Phase 1 drives it from a terminal UI so
// the audio path can be proven before any graphical shell exists. Since M4
// that interface is a client of the control API (ADR-0007 §4.1): it reaches
// the session only through InProcessEngine, as it will through a socket
// once M7 makes it a process of its own. This file is the wiring between
// the two and nothing else of the interface.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include "sonora/audio_backend.h"
#include "sonora/device_policy.h"
#include "sonora/engine_control_in_process.h"
#include "sonora/exit_watchdog.h"
#include "sonora/fake_backend.h"
#include "sonora/jack_backend.h"
#include "sonora/memory_lock.h"
#include "sonora/quit_signals.h"
#include "sonora/session.h"
#include "sonora/session_commands.h"
#include "sonora/terminal_ui.h"

namespace {

using sonora::backend::AudioBackend;

// The one place that names a concrete backend; everything below speaks
// AudioBackend. "fake" is drivers/fake on its own thread at real-time pace,
// shaped as the UMC1820, so the whole engine runs with no audio server;
// "fake:onboard" is a laptop's 2-input onboard audio instead.
std::unique_ptr<AudioBackend> make_backend(const std::string& name) {
    if (name == "jack") return std::make_unique<sonora::JackBackend>();
    if (name == "fake" || name == "fake:onboard") {
        sonora::FakeBackend::Config cfg = name == "fake"
                                              ? sonora::FakeBackend::Config{}
                                              : sonora::FakeBackend::onboard_config();
        cfg.mode = sonora::FakeBackend::Mode::Thread;
        return std::make_unique<sonora::FakeBackend>(cfg);
    }
    return nullptr;
}

// Process-wide setup a backend needs before any thread exists.
void prepare_backend(const std::string& name) {
    if (name == "jack") sonora::JackBackend::prepare_process();
}

// How long leaving may take once the take is closed. jack_deactivate() and
// jack_client_close() wait on the server, and a stalled one never answers.
constexpr unsigned kTeardownSeconds = 5;
constexpr const char* kTeardownHung =
    "sonora: the audio server did not let go within 5 seconds; exiting"
    " without it. The take was already closed.\n";

// Waits for the inputs to be connected. This is not a test accommodation:
// on the appliance the engine starts at boot, and a USB interface may not be
// enumerated yet, so the auto-connect is retried as the wait goes on rather
// than once at its end. A patch made by hand counts as well. Gives up early,
// with 0, once the backend has failed: nothing it connects will ever be read.
int wait_for_inputs(AudioBackend& audio, double seconds) {
    const int immediate = audio.connected_input_count();
    if (immediate > 0 || seconds <= 0.0) return immediate;

    std::printf("sonora: waiting up to %.0fs for inputs...\n", seconds);
    std::fflush(stdout);

    double waited = 0.0;
    int polls = 0;
    while (waited < seconds && !sonora::quit_requested() && !audio.fatal()) {
        ::usleep(100000);
        waited += 0.1;
        // Every half second: it lists the server's ports each time.
        if (++polls % 5 == 0) audio.connect_inputs_in_order();
        if (audio.connected_input_count() > 0) {
            // Give the rest of the ports a moment to land, so a 16-channel
            // interface does not start a take with two channels connected.
            ::usleep(500000);
            audio.connect_inputs_in_order();
            return audio.connected_input_count();
        }
    }
    return 0;
}

// After choose_capture_device(): jack_connect() returns before every server
// reports the link, so the count is read again for up to two seconds.
int connect_and_settle(AudioBackend& audio) {
    audio.connect_inputs_in_order();
    for (int i = 0; i < 20 && audio.connected_input_count() == 0; ++i) {
        ::usleep(100000);
    }
    return audio.connected_input_count();
}

// Records for a fixed duration and reports the result in one machine-readable
// line. Return code is the test verdict: anything non-zero means the recording
// is not trustworthy, even if files were produced.
int run_headless(sonora::Session& session, AudioBackend& audio,
                 double seconds, int connected, const std::string& device,
                 const sonora::MayRecord& may_record) {
    // Before the take opens: a server lost while waiting would otherwise
    // leave a set of 0-frame files behind it.
    if (audio.fatal()) {
        std::fprintf(stderr, "sonora: not recording: %s.\n",
                     audio.fatal_reason().c_str());
        return 1;
    }
    if (connected == 0) {
        std::fprintf(stderr,
                     "sonora: refusing to record with no connected inputs;"
                     " the result would be silence: %s.\n",
                     sonora::why_no_inputs(audio, device, session.tracks().size())
                         .c_str());
        return 1;
    }

    // Never take 1 unconditionally: a test run against a session folder that
    // already holds takes must add one, not replace one.
    std::string why;
    if (!may_record(why) || !session.start_recording(session.next_free_take(), &why)) {
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
        // Server loss, or a rate or buffer size change: the backend has
        // stopped feeding the session, so close the take and say why.
        if (audio.fatal()) {
            std::fprintf(stderr, "sonora: %s.\n", audio.fatal_reason().c_str());
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
    const uint32_t xruns = audio.xruns();
    const uint32_t steps = audio.clock_discontinuities();
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

    // A clock step puts no hole in the audio, so it is reported rather than
    // failed: the take's timestamps after it do not follow on from before.
    std::printf("RESULT files=%d connected=%d frames=%llu requested=%llu"
                " rate=%u buffer=%u xruns=%u clock_steps=%u overruns=%u\n",
                files, connected,
                static_cast<unsigned long long>(frames_on_disk),
                static_cast<unsigned long long>(target),
                audio.sample_rate(), audio.buffer_frames(), xruns, steps, overruns);

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
    std::string capture_device;
    std::string backend_name = "jack";

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
        } else if (std::strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            capture_device = argv[++i];
        } else if (std::strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            backend_name = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf(
                "usage: sonora-engine [options]\n"
                "  --dir PATH        session directory\n"
                "  --tracks N|auto   track count, or one per capture channel\n"
                "  --device NAME     capture device to record from. Without it,\n"
                "                    the first line of ~/.config/sonora/device or\n"
                "                    /etc/sonora/device; without those, the one USB\n"
                "                    audio interface or, once the wait is over with\n"
                "                    none, the one onboard device\n"
                "  --backend NAME    jack (default), or fake: no audio server, an\n"
                "                    18-in UMC1820 with a distinct tone per channel\n"
                "                    (fake:onboard: 2-in onboard audio instead)\n"
                "  --rate HZ         session sample rate (default 48000)\n"
                "  --float           record 32-bit float instead of 24-bit\n"
                "  --arm-all         arm every track at startup\n"
                "  --keymap PATH     key map to use instead of the built-in one\n"
                "  --record SECONDS  record for SECONDS, then exit (headless)\n"
                "  --wait-for-inputs SECONDS\n"
                "                    wait for a capture connection, or a USB\n"
                "                    interface, before recording\n");
            return 0;
        } else {
            std::fprintf(stderr, "sonora: unknown option %s\n", argv[i]);
            return 2;
        }
    }

    // Before anything else is set up: a misspelled backend is a usage error.
    if (!make_backend(backend_name)) {
        std::fprintf(stderr, "sonora: unknown backend %s (jack, fake, fake:onboard)\n",
                     backend_name.c_str());
        return 2;
    }

    if (record_seconds > 0.0 && !arm_all) {
        // Recording nothing for ten seconds and exiting 0 is a test that
        // passes while proving nothing. Refuse it.
        std::fprintf(stderr,
                     "sonora: --record needs at least one armed track;"
                     " pass --arm-all.\n");
        return 2;
    }

    // The appliance's unit passes no --device, so a choice that has to
    // survive a reboot lives in a file the operator can edit instead.
    if (capture_device.empty()) {
        std::string from;
        capture_device = sonora::read_device_setting(
            sonora::device_setting_paths(std::getenv("HOME"), "/etc/sonora"), &from);
        if (!capture_device.empty()) {
            std::printf("sonora: capture device \"%s\", from %s\n",
                        capture_device.c_str(), from.c_str());
        }
    }
    // Named, the device is never replaced by the default policy's choice.
    const bool device_named = !capture_device.empty();
    // Set when the default fell back to a non-USB device, for the screen.
    std::string onboard_notice;

    // First, while this is the only thread: it may set the environment.
    prepare_backend(backend_name);

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

    // Before the session's rings and before any JACK client and its threads
    // exist, so all of them are locked as they are mapped. Refused rather
    // than warned about: on the appliance the unit grants unlimited memlock,
    // so a failure means the image is wrong, and a page fault on the audio
    // thread shows up only as xruns nobody can explain.
    {
        std::string why;
        if (!sonora::lock_memory(why)) {
            std::fprintf(stderr,
                         "sonora: cannot lock memory: %s. Refusing to start:"
                         " the audio thread could page-fault mid-take.\n",
                         why.c_str());
            return 1;
        }
    }

    double wait_left = wait_for_inputs_seconds;
    if (cfg.track_count == 0) {
        // One track per capture channel of the device recorded from. Guessing
        // a number means either silent tracks or inputs nobody is recording.
        // The session is sized before it exists, so the startup wait for a
        // late interface, and the fallback after it, happen here. Only the
        // chosen device counts: onboard audio's two ports are not channels
        // of the interface, nor the interface's eighteen of onboard audio.
        // A backend of its own, gone before the session's is opened.
        const std::unique_ptr<AudioBackend> probe = make_backend(backend_name);
        std::vector<sonora::backend::PortInfo> all, ports;
        std::string why;
        bool said = false;
        for (double waited = 0.0;; waited += 0.5) {
            const bool over = waited >= wait_for_inputs_seconds || sonora::quit_requested();
            all = probe->physical_capture_ports();
            if (!sonora::backend::select_device(all, capture_device, ports, why, over)) {
                std::fprintf(stderr, "sonora: --tracks auto: %s\n", why.c_str());
                return 1;
            }
            if (!ports.empty() || over) {
                wait_left = std::max(0.0, wait_for_inputs_seconds - waited);
                break;
            }
            if (!said) {
                std::printf("sonora: waiting up to %.0fs for %s...\n",
                            wait_for_inputs_seconds,
                            device_named ? capture_device.c_str() : "a USB interface");
                std::fflush(stdout);
                said = true;
            }
            ::usleep(500000);
        }
        const int channels = static_cast<int>(ports.size());
        if (channels == 0) {
            std::fprintf(stderr, "sonora: --tracks auto found no capture channels: %s.\n",
                         sonora::backend::describe_absent(all, capture_device).c_str());
            return 1;
        }
        cfg.track_count = channels;
        // Pinned by name, so a second card appearing before open() cannot
        // change the device the session was sized to.
        capture_device = ports.front().device;
        if (!device_named && !ports.front().usb) {
            onboard_notice = sonora::onboard_notice(capture_device);
            std::fprintf(stderr, "sonora: %s\n", onboard_notice.c_str());
        }
        std::printf("sonora: %d capture channels detected on %s\n", channels,
                    capture_device.c_str());
    }

    sonora::Session session(cfg);
    if (!session.create_dirs()) {
        std::fprintf(stderr, "sonora: cannot create session at %s\n",
                     cfg.dir.c_str());
        return 1;
    }

    // Declared after the session, so it is destroyed first: its destructor
    // deactivates, and the callback never outlives what it calls into.
    const std::unique_ptr<AudioBackend> audio = make_backend(backend_name);
    std::string backend_error;
    // Destroyed before `audio`, so every way out from here bounds the
    // backend's teardown; the explicit deactivate() calls below arm it first
    // themselves.
    struct BoundedTeardown {
        ~BoundedTeardown() { sonora::arm_exit_watchdog(kTeardownSeconds, kTeardownHung); }
    } const bounded_teardown;
    // open() refuses a server at any rate but the session's, since a
    // mismatch would write wrong-pitch files, and refuses to guess between
    // two USB interfaces.
    if (!audio->open({"sonora", static_cast<uint32_t>(cfg.track_count), 0,
                      cfg.sample_rate, capture_device},
                     session, backend_error) ||
        !audio->activate(backend_error)) {
        std::fprintf(stderr, "sonora: %s\n", backend_error.c_str());
        return 1;
    }
    audio->connect_inputs_in_order();
    int connected = wait_for_inputs(*audio, wait_left);
    // The wait is over with nothing connected, by the interface or by hand:
    // the laptop's own audio, if it is the only device, rather than an
    // engine that records nothing until the interface it may never get.
    if (connected == 0 && !audio->fatal() && capture_device.empty()) {
        const sonora::Fallback fallback = sonora::fall_back_after_wait(*audio);
        if (!fallback.ok) {
            std::fprintf(stderr, "sonora: %s.\n", fallback.error.c_str());
            return 1;
        }
        if (!fallback.device.empty()) {
            capture_device = fallback.device;
            onboard_notice = fallback.notice;
            if (!onboard_notice.empty()) {
                std::fprintf(stderr, "sonora: %s\n", onboard_notice.c_str());
            }
            connected = connect_and_settle(*audio);
        }
    }
    // Every track would record silence. Recording nothing while saying
    // nothing is the one outcome the operator must never get.
    if (connected == 0 && !audio->fatal()) {
        const std::string why =
            sonora::why_no_inputs(*audio, capture_device, session.tracks().size());
        // A device named and still absent after the wait is most likely
        // misspelled, and waiting longer will not fix that. Nor will a
        // choice between interfaces that nobody made.
        std::vector<sonora::backend::PortInfo> chosen;
        std::string several;
        if (device_named ||
            !sonora::backend::select_device(audio->physical_capture_ports(), "",
                                            chosen, several)) {
            std::fprintf(stderr, "sonora: %s.\n", why.c_str());
            return 1;
        }
        // Headless says it once, as its refusal; interactive draws it.
        if (record_seconds <= 0.0) {
            std::fprintf(stderr, "sonora: no capture ports are connected: %s.\n",
                         why.c_str());
        }
    }
    session.disk().start();

    // Before every take, headless or from a key: when no armed track has
    // anything connected, every file would be silence.
    const sonora::MayRecord may_record = [&session, &audio,
                                          &capture_device](std::string& why) {
        bool armed = false;
        for (std::size_t i = 0; i < session.tracks().size(); ++i) {
            if (!session.tracks()[i]->rec_armed()) continue;
            armed = true;
            if (audio->input_connected(static_cast<uint32_t>(i))) return true;
        }
        // start_recording() says "no track is armed" better than this would.
        if (!armed) return true;
        why = "no armed track has an input connected (" +
              sonora::why_no_inputs(*audio, capture_device, session.tracks().size()) +
              ")";
        return false;
    };

    if (arm_all) {
        for (const auto& tr : session.tracks()) tr->set_rec_armed(true);
    }

    if (record_seconds > 0.0) {
        const int rc = run_headless(session, *audio, record_seconds, connected,
                                    capture_device, may_record);
        sonora::arm_exit_watchdog(kTeardownSeconds, kTeardownHung);
        audio->deactivate();
        return rc;
    }

    // What the old terminal loop checked once a second, now on the engine's
    // control thread: while nothing is connected the interface is looked
    // for, so one plugged in after the start-up wait is still picked up,
    // and the interface is told what is missing for as long as it is.
    std::string inputs_missing_said = "(unset)";
    bool late_usb_said = false;
    sonora::EngineControlServer::Options control;
    control.may_record = may_record;
    control.housekeeping = [&](sonora::EngineControlServer& server) {
        const auto notice = [&server](sonora::api::EventKind kind, const std::string& text) {
            sonora::api::EngineEvent e;
            e.kind = kind;
            e.text = text;
            server.post_event(e);
        };
        std::string fallback_error;
        if (audio->connected_input_count() == 0) {
            audio->connect_inputs_in_order();
            // A device that appeared only after the wait gets the same
            // fallback; several are named on screen instead of exiting, so
            // the unit does not restart into the same choice.
            if (audio->connected_input_count() == 0 && capture_device.empty()) {
                const sonora::Fallback f = sonora::fall_back_after_wait(*audio);
                fallback_error = f.error;
                if (!f.notice.empty()) {
                    capture_device = f.device;
                    onboard_notice = f.notice;
                    notice(sonora::api::EventKind::Notice, onboard_notice);
                    std::fprintf(stderr, "sonora: %s\n", onboard_notice.c_str());
                }
                if (!f.device.empty()) audio->connect_inputs_in_order();
            }
        }
        const int connected_now = audio->connected_input_count();
        const std::string missing =
            connected_now != 0 ? std::string()
            : !fallback_error.empty()
                ? fallback_error
                : sonora::why_no_inputs(*audio, capture_device, session.tracks().size());
        if (missing != inputs_missing_said) {
            notice(sonora::api::EventKind::InputsMissing, missing);
            inputs_missing_said = missing;
        }
        server.set_inputs_connected(static_cast<uint32_t>(connected_now), true);
        for (std::size_t i = 0; i < session.tracks().size(); ++i) {
            server.set_input_connected(static_cast<uint32_t>(i),
                                       audio->input_connected(static_cast<uint32_t>(i)));
        }
        // On onboard, an interface plugged in later is not switched to.
        if (!onboard_notice.empty() && !late_usb_said) {
            const std::string late =
                sonora::late_usb_notice(audio->physical_capture_ports());
            if (!late.empty()) {
                notice(sonora::api::EventKind::Notice, late);
                std::fprintf(stderr, "sonora: %s\n", late.c_str());
                late_usb_said = true;
            }
        }
    };

    sonora::InProcessEngine engine(session, audio.get(), std::move(control));
    // Kept on screen for the whole session: the operator must not take the
    // laptop's microphone for the interface.
    if (!onboard_notice.empty()) {
        sonora::api::EngineEvent e;
        e.kind = sonora::api::EventKind::Notice;
        e.text = onboard_notice;
        engine.server().post_event(e);
    }
    {
        std::string error;
        if (!engine.connect(error)) {
            std::fprintf(stderr, "sonora: the terminal interface cannot reach the engine: %s\n",
                         error.c_str());
            session.stop();
            sonora::arm_exit_watchdog(kTeardownSeconds, kTeardownHung);
            audio->deactivate();
            return 1;
        }
    }

    sonora::tui::TerminalView view(engine, keymap);
    sonora::tui::TerminalOptions terminal;
    // A SIGTERM or hangup ends the session as cleanly as a quit key.
    terminal.stop_requested = []() { return sonora::quit_requested(); };
    const sonora::tui::TerminalExit exit = sonora::tui::run_terminal(view, terminal);

    // No command runs while the take closes.
    engine.shutdown();
    // stop() says on stderr why a take failed, and the terminal is already
    // the operator's again.
    const bool take_ok = session.stop();
    sonora::arm_exit_watchdog(kTeardownSeconds, kTeardownHung);
    audio->deactivate();
    std::printf("sonora: session written to %s\n", cfg.dir.c_str());

    // The server dying, or changing rate or buffer size, stops the callback
    // without stopping us. The take is closed, so what was captured is a
    // valid file; the service unit restarts us against a fresh server.
    if (exit == sonora::tui::TerminalExit::EngineFailed || audio->fatal()) {
        std::fprintf(stderr, "sonora: %s.\n", audio->fatal_reason().c_str());
        return 1;
    }
    // A quit or SIGTERM in the middle of a failed take is not a clean exit.
    return take_ok ? 0 : 1;
}
