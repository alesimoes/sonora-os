// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-engine: the recording core, a headless process (ADR-0007 §3, M7).
// Every interface is a client on its control socket: the Qt UI, sonora-tui,
// tools. On a terminal it starts sonora-tui beside itself as a child
// process, so the appliance's tty still shows an interface; the engine and
// the take do not depend on that child, which is restarted if it dies.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#include "sonora/api/state_block.h"
#include "sonora/audio_backend.h"
#include "sonora/closed_take.h"
#include "sonora/control_socket_server.h"
#include "sonora/device_policy.h"
#include "sonora/engine_control_in_process.h"
#include "sonora/engine_control_server.h"
#include "sonora/exit_watchdog.h"
#include "sonora/fake_backend.h"
#include "sonora/jack_backend.h"
#include "sonora/memory_lock.h"
#include "sonora/platform/file.h"
#include "sonora/platform/ipc.h"
#include "sonora/platform/process.h"
#include "sonora/plugin_paths.h"
#include "sonora/quit_signals.h"
#include "sonora/session.h"
#include "sonora/session_commands.h"

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

// --measure-latency: the round trip from out_<output> to in_<input>, which
// the operator has cabled together, before anything records (ADR-0019 §4).
// Measured, it is the session's L; otherwise L stays as it was and the
// operator is told why. 1-based, as the ports are named.
void measure_latency(sonora::Session& session, uint32_t output, uint32_t input) {
    sonora::LatencyProbe& probe = session.latency_probe();
    sonora::LatencyProbe::Settings settings;
    settings.output = output - 1;
    settings.input = input - 1;
    const uint32_t rate = session.config().sample_rate;
    const std::string which =
        "--measure-latency out_" + std::to_string(output) + " to in_" + std::to_string(input);
    std::string error;
    if (!probe.start(settings, &error)) {
        std::fprintf(stderr, "sonora: %s: %s\n", which.c_str(), error.c_str());
        return;
    }
    // Two windows a ping, and three more for each restart it allows, then
    // two seconds for the server to start the periods.
    const uint64_t window = settings.window != 0 ? settings.window : rate / 4;
    const uint64_t frames = (2 * settings.pings + 3 * (settings.pings + 2)) * window;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(1000 * frames / std::max<uint32_t>(rate, 1) + 2000);
    sonora::LatencyProbe::Result result{};
    while (!probe.poll(result)) {
        if (std::chrono::steady_clock::now() > deadline) {
            probe.cancel();
            std::fprintf(stderr, "sonora: %s: no result; is the audio server running periods?\n",
                         which.c_str());
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const std::string said = sonora::LatencyProbe::describe(result, rate);
    if (result.outcome != sonora::LatencyProbe::Outcome::Measured) {
        std::fprintf(stderr, "sonora: %s: %s. Takes are compensated by %u frames.\n", which.c_str(),
                     said.c_str(), session.input_latency());
        return;
    }
    if (!session.set_input_latency(result.frames, &error)) {
        std::fprintf(stderr, "sonora: %s: %s: %s\n", which.c_str(), said.c_str(), error.c_str());
        return;
    }
    std::fprintf(stderr, "sonora: %s: %s. Takes are compensated by it.\n", which.c_str(),
                 said.c_str());
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

// The reply to `seq`, polling the engine as an interface would, for up to
// `seconds`. False if none came.
bool await_reply(sonora::InProcessEngine& engine, uint64_t seq, double seconds,
                 sonora::api::CommandReply& out) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    sonora::api::Inbox in;
    while (std::chrono::steady_clock::now() < end && !sonora::quit_requested()) {
        engine.poll(in);
        for (const auto& r : in.replies) {
            if (r.seq == seq) {
                out = r;
                return true;
            }
        }
        in.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

sonora::api::CommandReply run_command(sonora::InProcessEngine& engine, const std::string& id,
                                      int32_t target, double seconds) {
    sonora::api::CommandRequest r;
    r.command = id;
    r.target = target;
    sonora::api::CommandReply reply;
    reply.result = sonora::DispatchResult::Failed;
    if (!await_reply(engine, engine.send(r), seconds, reply)) {
        reply.result = sonora::DispatchResult::Failed;
        reply.message = "no reply within " + std::to_string(static_cast<int>(seconds)) + " s";
    }
    return reply;
}

// Records for a fixed duration and reports the result in one machine-readable
// line. Return code is the test verdict: anything non-zero means the recording
// is not trustworthy, even if files were produced.
//
// Driven through the control server with no socket, as an interface would
// drive it (ADR-0009, update "playback on the M4/M7 control server", item
// 5): arming, the take behind the residency gate, and the stop, so what is
// heard during the take is what --place put in the server's document.
int run_headless(sonora::Session& session, AudioBackend& audio, sonora::InProcessEngine& engine,
                 double seconds, int connected, const std::string& device, bool arm_all,
                 const std::vector<int>& arm_tracks) {
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

    std::string why;
    if (!engine.connect(why)) {
        std::fprintf(stderr, "sonora: the control server did not start: %s\n", why.c_str());
        return 1;
    }
    // Commands are answered in milliseconds; a take waits for its window
    // to be resident, a few seconds at worst on a slow disk.
    constexpr double kReplySeconds = 5.0;
    constexpr double kTakeSeconds = 15.0;
    if (arm_all) {
        const auto r = run_command(engine, "track.arm_all", -1, kReplySeconds);
        if (r.result != sonora::DispatchResult::Ok) {
            std::fprintf(stderr, "sonora: cannot arm: %s\n", r.message.c_str());
            return 1;
        }
    } else {
        for (const int t : arm_tracks) {
            if (session.tracks()[static_cast<std::size_t>(t)]->rec_armed()) continue;
            const auto r = run_command(engine, "track.toggle_arm", t, kReplySeconds);
            if (r.result != sonora::DispatchResult::Ok) {
                std::fprintf(stderr, "sonora: cannot arm track %d: %s\n", t + 1,
                             r.message.c_str());
                return 1;
            }
        }
    }

    // Never take 1 unconditionally: the server numbers the take from the
    // folder, so a run against a session that already holds takes adds one.
    // The reply comes once the take has started, or with why it did not.
    const auto rec = run_command(engine, "transport.record", -1, kTakeSeconds);
    if (rec.result != sonora::DispatchResult::Ok) {
        std::fprintf(stderr, "sonora: cannot record: %s\n", rec.message.c_str());
        return 1;
    }
    // The take starts at the playhead, which stands still until it does.
    const uint64_t origin = session.transport().playhead();

    const uint64_t target =
        static_cast<uint64_t>(seconds * session.transport().sample_rate());

    // Wall-clock bound as well as a sample bound: if the server stops calling
    // the callback the playhead stops advancing, and waiting on samples alone
    // would hang forever instead of failing.
    const double deadline = seconds * 2.0 + 5.0;
    double waited = 0.0;

    const auto abandon = [&session, &engine]() {
        engine.shutdown();
        session.stop();
    };
    while (!sonora::quit_requested() &&
           session.transport().playhead() - origin < target) {
        // Server loss, or a rate or buffer size change: the backend has
        // stopped feeding the session, so close the take and say why.
        if (audio.fatal()) {
            std::fprintf(stderr, "sonora: %s.\n", audio.fatal_reason().c_str());
            abandon();
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
                             session.transport().playhead() - origin),
                         static_cast<unsigned long long>(target));
            abandon();
            return 1;
        }
    }

    const auto stop = run_command(engine, "transport.stop", -1, kTakeSeconds);
    engine.shutdown();
    if (stop.result != sonora::DispatchResult::Ok) {
        std::fprintf(stderr, "sonora: %s\n", stop.message.c_str());
        session.stop();
        return 1;
    }

    // Read the counters after the stop, so the disk thread has flushed and
    // the overrun total is final rather than a snapshot mid-drain.
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
    // A playback miss is a hole in what was heard during the take.
    // checkpoint_ms is the longest the rings waited on one take's sync, the
    // figure the ring's depth must cover (session-format.md, "Crash
    // recovery"); last, so the fields before it parse as they always have.
    const uint64_t misses = session.playback_misses();
    const uint64_t dropped = session.dropped_samples();
    std::printf("RESULT files=%d connected=%d frames=%llu requested=%llu"
                " rate=%u buffer=%u xruns=%u clock_steps=%u overruns=%u misses=%llu"
                " dropped=%llu checkpoints=%u checkpoint_ms=%.1f\n",
                files, connected,
                static_cast<unsigned long long>(frames_on_disk),
                static_cast<unsigned long long>(target),
                audio.sample_rate(), audio.buffer_frames(), xruns, steps, overruns,
                static_cast<unsigned long long>(misses),
                static_cast<unsigned long long>(dropped),
                static_cast<unsigned>(session.disk().checkpoints()),
                static_cast<double>(session.disk().longest_checkpoint_ns()) / 1e6);

    if (files == 0) {
        std::fprintf(stderr, "sonora: no audio was written.\n");
        return 1;
    }
    if (misses > 0) {
        std::fprintf(stderr,
                     "sonora: %llu playback chunks were not resident when played.\n",
                     static_cast<unsigned long long>(misses));
        return 1;
    }
    if (dropped > 0) {
        std::fprintf(stderr,
                     "sonora: %llu recorded samples reached no file; the audio"
                     " callback outlasted the stop.\n",
                     static_cast<unsigned long long>(dropped));
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
    // Tracks armed one by one, 0-based; and what --place puts on track 1, 2...
    std::vector<int> arm_tracks;
    std::vector<std::string> place_files;
    bool no_input_monitoring = false;
    // ADR-0019 §4: L given, and the cabled pair to measure it on (0: none).
    long input_latency = -1;
    uint32_t probe_output = 0;
    uint32_t probe_input = 0;
    std::string keymap_path;
    std::string capture_device;
    std::string backend_name = "jack";
    std::string control_socket;
    std::string tui_path;
    bool headless = false;
    bool lock = true;
    sonora::EngineControlServer::Options control;
    bool plugin_cache_given = false;

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
        } else if (std::strcmp(argv[i], "--arm") == 0 && i + 1 < argc) {
            const int n = std::atoi(argv[++i]);
            if (n < 1) {
                std::fprintf(stderr, "sonora: --arm takes a track number from 1\n");
                return 2;
            }
            if (std::find(arm_tracks.begin(), arm_tracks.end(), n - 1) == arm_tracks.end()) {
                arm_tracks.push_back(n - 1);
            }
        } else if (std::strcmp(argv[i], "--place") == 0 && i + 1 < argc) {
            place_files.emplace_back(argv[++i]);
        } else if (std::strcmp(argv[i], "--no-input-monitoring") == 0) {
            no_input_monitoring = true;
        } else if (std::strcmp(argv[i], "--input-latency") == 0 && i + 1 < argc) {
            input_latency = std::atol(argv[++i]);
            if (input_latency < 0) {
                std::fprintf(stderr, "sonora: --input-latency takes a frame count from 0\n");
                return 2;
            }
        } else if (std::strcmp(argv[i], "--measure-latency") == 0 && i + 1 < argc) {
            unsigned out = 0, in = 0;
            char rest = 0;
            if (std::sscanf(argv[++i], "%u:%u%c", &out, &in, &rest) != 2 || out == 0 || in == 0) {
                std::fprintf(stderr, "sonora: --measure-latency takes OUT:IN, port numbers from 1\n");
                return 2;
            }
            probe_output = out;
            probe_input = in;
        } else if (std::strcmp(argv[i], "--plugin-host") == 0 && i + 1 < argc) {
            control.plugin_host = argv[++i];
        } else if (std::strcmp(argv[i], "--plugin-bundle") == 0 && i + 1 < argc) {
            control.plugin_bundles.emplace_back(argv[++i]);
        } else if (std::strcmp(argv[i], "--plugin-cache") == 0 && i + 1 < argc) {
            control.plugin_cache = argv[++i];
            plugin_cache_given = true;
        } else if (std::strcmp(argv[i], "--keymap") == 0 && i + 1 < argc) {
            keymap_path = argv[++i];
        } else if (std::strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            capture_device = argv[++i];
        } else if (std::strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            backend_name = argv[++i];
        } else if (std::strcmp(argv[i], "--control-socket") == 0 && i + 1 < argc) {
            control_socket = argv[++i];
        } else if (std::strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (std::strcmp(argv[i], "--tui") == 0 && i + 1 < argc) {
            tui_path = argv[++i];
        } else if (std::strcmp(argv[i], "--no-memlock") == 0) {
            lock = false;
        } else if (std::strcmp(argv[i], "--midi-max-entries") == 0 && i + 1 < argc) {
            const long long n = std::atoll(argv[++i]);
            if (n < 1) {
                std::fprintf(stderr, "sonora: --midi-max-entries takes a count from 1\n");
                return 2;
            }
            control.midi_max_entries = static_cast<uint64_t>(n);
        } else if (std::strcmp(argv[i], "--announce-versions") == 0 && i + 2 < argc) {
            control.protocol_version = static_cast<uint32_t>(std::atoi(argv[++i]));
            control.state_layout_version = static_cast<uint32_t>(std::atoi(argv[++i]));
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
                "  --arm N           arm track N at startup (repeatable)\n"
                "  --place FILE      play FILE (relative to --dir) on track 1, at\n"
                "                    its BWF time reference (repeatable: tracks 2, 3...)\n"
                "  --no-input-monitoring\n"
                "                    armed tracks send nothing to the monitor bus\n"
                "  --input-latency FRAMES\n"
                "                    the round trip out and back in; takes are placed\n"
                "                    that much earlier (default 0)\n"
                "  --measure-latency OUT:IN\n"
                "                    measure it on out_OUT, cabled to in_IN, at start\n"
                "                    (impulses at -20 dBFS on out_OUT)\n"
                "  --plugin-host PATH  the plugin host (default: sonora-plugin-host\n"
                "                    beside this binary; none: inserts show faulted)\n"
                "  --plugin-bundle DIR  an extra LV2/VST3 bundle directory (repeatable)\n"
                "  --plugin-cache PATH  the plugin catalog cache (default:\n"
                "                    ~/.cache/sonora/plugin-catalog.json; \"\": in memory)\n"
                "  --keymap PATH     key map for the terminal interface\n"
                "  --midi-max-entries N\n"
                "                    MIDI notes and events a session may hold, undo\n"
                "                    history included (default 16777216); a session\n"
                "                    over it opens only once this is raised\n"
                "  --record SECONDS  record for SECONDS, then exit (headless)\n"
                "  --wait-for-inputs SECONDS\n"
                "                    wait for a capture connection, or a USB\n"
                "                    interface, before recording\n"
                "  --control-socket PATH\n"
                "                    where interfaces connect (default\n"
                "                    $XDG_RUNTIME_DIR/sonora/engine.sock)\n"
                "  --headless        start no terminal interface, even on a tty\n"
                "  --tui PATH        the terminal interface to start (default:\n"
                "                    sonora-tui beside this program)\n"
                "  --no-memlock      development only: run without locked memory;\n"
                "                    the audio thread may page-fault (xruns)\n"
                "  --announce-versions PROTOCOL LAYOUT\n"
                "                    announce other versions in Hello, for the\n"
                "                    conformance suite's mismatch check\n");
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

    if (record_seconds > 0.0 && !arm_all && arm_tracks.empty()) {
        // Recording nothing for ten seconds and exiting 0 is a test that
        // passes while proving nothing. Refuse it.
        std::fprintf(stderr,
                     "sonora: --record needs at least one armed track;"
                     " pass --arm-all or --arm N.\n");
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

    // Before anything touches hardware: a second engine on the same socket
    // is refused while it can still leave without having opened a device.
    // The headless --record mode has no interface and no socket.
    std::unique_ptr<sonora::platform::ControlListener> listener;
    if (record_seconds <= 0.0) {
        if (control_socket.empty()) control_socket = sonora::platform::default_control_endpoint();
        std::string err;
        listener = sonora::platform::ControlListener::listen(control_socket, err);
        if (!listener) {
            std::fprintf(stderr, "sonora: cannot open the control socket: %s\n", err.c_str());
            return 1;
        }
    }

    // Before the session's rings and before any JACK client and its threads
    // exist, so all of them are locked as they are mapped. Refused rather
    // than warned about: on the appliance the unit grants unlimited memlock,
    // so a failure means the image is wrong, and a page fault on the audio
    // thread shows up only as xruns nobody can explain. --no-memlock is for
    // a developer's machine (WSL's default limit is 64 KiB), and says so.
    if (!lock) {
        std::fprintf(stderr,
                     "sonora: --no-memlock: memory is not locked; the audio thread may"
                     " page-fault and drop out. Never on the appliance.\n");
    } else {
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
    // Before recovery: a second engine on the folder would treat the first
    // one's open takes as unfinished and cut the frame being written. Held
    // until the process exits. The control socket does not cover this:
    // --record opens none, and --socket can name another.
    sonora::platform::FolderLock session_lock;
    {
        std::string err;
        switch (session_lock.acquire(cfg.dir, err)) {
            case sonora::platform::FolderLock::Result::Held:
                break;
            case sonora::platform::FolderLock::Result::Busy:
                std::fprintf(stderr,
                             "sonora: %s; one session takes one engine. Stop the other"
                             " engine first.\n",
                             err.c_str());
                return 1;
            case sonora::platform::FolderLock::Result::Failed:
                std::fprintf(stderr, "sonora: %s\n", err.c_str());
                return 1;
        }
    }
    // Before anything records: a take an earlier engine died recording
    // (kill -9, a crash, power) gets the length that reached the disk.
    const std::vector<std::string> recovered =
        sonora::recover_unclosed_takes(cfg.dir, cfg.sample_rate);
    for (const std::string& line : recovered) {
        std::fprintf(stderr, "sonora: %s\n", line.c_str());
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
    // Two outputs: the fixed stereo monitor bus (ADR-0009 §4.6), and as far
    // as the latency probe's if it is past them.
    if (probe_input > static_cast<uint32_t>(cfg.track_count)) {
        std::fprintf(stderr, "sonora: --measure-latency: there is no in_%u with %d tracks\n",
                     probe_input, cfg.track_count);
        return 2;
    }
    if (!audio->open({"sonora", static_cast<uint32_t>(cfg.track_count),
                      std::max<uint32_t>(2, probe_output), cfg.sample_rate, capture_device},
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
    // Before the first take can start: L is fixed during one.
    if (input_latency >= 0) {
        std::string why;
        if (!session.set_input_latency(static_cast<uint32_t>(std::min<long>(input_latency, 0xFFFFFFFFL)),
                                       &why)) {
            std::fprintf(stderr, "sonora: --input-latency: %s\n", why.c_str());
            return 2;
        }
    }
    if (probe_output != 0 && !audio->fatal()) measure_latency(session, probe_output, probe_input);
    session.disk().start();
    // Separate from the capture writer: a slow read never delays a drain.
    session.reader().start();

    for (const int t : arm_tracks) {
        if (static_cast<std::size_t>(t) >= session.tracks().size()) {
            std::fprintf(stderr, "sonora: --arm %d: there are %zu tracks\n", t + 1,
                         session.tracks().size());
            return 2;
        }
    }
    // What --place puts on the timeline is where the server's one document
    // starts, so it is what plays, what every interface shows and what an
    // edit changes.
    if (!place_files.empty()) {
        sonora::model::Document document(sonora::engine_document(session));
        std::vector<sonora::model::ReplicationUpdate> placed;
        std::string why;
        for (std::size_t i = 0; i < place_files.size(); ++i) {
            if (!sonora::place_file(document, cfg.dir, place_files[i], i, placed, why)) {
                std::fprintf(stderr, "sonora: --place: %s\n", why.c_str());
                return 2;
            }
        }
        control.initial_document = std::make_shared<sonora::model::Snapshot>(document.current());
    }
    control.input_monitoring = !no_input_monitoring;

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

    if (record_seconds > 0.0) {
        control.may_record = may_record;
        sonora::InProcessEngine engine(session, audio.get(), std::move(control));
        const int rc = run_headless(session, *audio, engine, record_seconds, connected,
                                    capture_device, arm_all, arm_tracks);
        engine.shutdown();
        sonora::arm_exit_watchdog(kTeardownSeconds, kTeardownHung);
        audio->deactivate();
        engine.server().after_deactivate();
        return rc;
    }

    // Before the server exists, so the mix it applies at start already
    // sees them armed (--no-input-monitoring).
    if (arm_all) {
        for (const auto& tr : session.tracks()) tr->set_rec_armed(true);
    }
    for (const int t : arm_tracks) {
        session.tracks()[static_cast<std::size_t>(t)]->set_rec_armed(true);
    }

    // What the old terminal loop checked once a second, now on the engine's
    // control thread: while nothing is connected the interface is looked
    // for, so one plugged in after the start-up wait is still picked up,
    // and the interface is told what is missing for as long as it is.
    std::string inputs_missing_said = "(unset)";
    bool late_usb_said = false;
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

    // The state block in shared memory that every interface maps read-only.
    // Sized here, once, for the session's tracks.
    std::string region_error;
    const std::unique_ptr<sonora::platform::SharedRegion> state =
        sonora::platform::SharedRegion::create(
            "sonora-state",
            sonora::api::state_block_bytes(static_cast<uint32_t>(session.tracks().size())),
            region_error);
    if (!state) {
        std::fprintf(stderr, "sonora: cannot create the shared state block: %s\n",
                     region_error.c_str());
        session.stop();
        sonora::arm_exit_watchdog(kTeardownSeconds, kTeardownHung);
        audio->deactivate();
        return 1;
    }
    control.state_memory = state->writable_data();
    control.state_memory_bytes = state->bytes();
    // The plugin host beside this binary, as the image installs it; without
    // one the engine still records and plays, every insert shown faulted.
    if (control.plugin_host.empty()) {
        const std::string dir = sonora::platform::executable_dir();
        const std::string host = dir.empty() ? std::string() : dir + "/sonora-plugin-host";
        if (!host.empty() && ::access(host.c_str(), X_OK) == 0) control.plugin_host = host;
    }
    // The catalog cache (ADR-0012, amendment of 2026-10-19, change A and
    // Q2): resolved here and passed down; an empty --plugin-cache keeps
    // the catalog in memory.
    if (!plugin_cache_given) control.plugin_cache = sonora::default_plugin_cache(sonora::platform::user_cache_dir());
    // Windows plug-ins through yabridge (ADR-0017 Y4): the prefix their
    // probes and chains run in, created before the first scan that lists
    // one.
    control.plugin_wine_prefix = sonora::default_wine_prefix(std::getenv("WINEPREFIX"), std::getenv("HOME"));
    control.plugin_wine_prefix_command = sonora::wine_prefix_command(std::getenv("PATH"));
    {
        std::ifstream os_release("/etc/os-release");
        std::ostringstream text;
        text << os_release.rdbuf();
        control.plugin_build_id = sonora::image_build_id(text.str());
    }

    sonora::EngineControlServer server(session, audio.get(), std::move(control));
    // Kept on screen for the whole session, by every interface that ever
    // connects: the operator must not take the laptop's microphone for the
    // interface.
    if (!onboard_notice.empty()) {
        sonora::api::EngineEvent e;
        e.kind = sonora::api::EventKind::Notice;
        e.text = onboard_notice;
        server.post_event(e);
    }
    for (const std::string& line : recovered) {
        sonora::api::EngineEvent e;
        e.kind = sonora::api::EventKind::Notice;
        e.text = line;
        server.post_event(e);
    }
    server.start();
    sonora::ControlSocketServer sockets(server, *state, std::move(listener));
    sockets.start();
    std::printf("sonora: engine listening on %s\n", sockets.endpoint().c_str());
    std::fflush(stdout);

    // On a terminal, and unless told otherwise, the terminal interface: a
    // child process on the same tty, so the appliance's console still has
    // one. Its quit key ends the session; its crash does not, and it is
    // started again while the take goes on.
    if (tui_path.empty()) {
        const std::string dir = sonora::platform::executable_dir();
        if (!dir.empty()) tui_path = dir + "/sonora-tui";
    }
    const bool want_tui = !headless && ::isatty(STDIN_FILENO) == 1;
    const bool have_tui = !tui_path.empty() && ::access(tui_path.c_str(), X_OK) == 0;
    if (want_tui && !have_tui) {
        std::fprintf(stderr,
                     "sonora: no terminal interface at %s; running headless. Interfaces"
                     " connect to %s.\n",
                     tui_path.c_str(), sockets.endpoint().c_str());
    }
    std::vector<std::string> tui_argv = {tui_path, "--control-socket", sockets.endpoint()};
    if (!keymap_path.empty()) {
        tui_argv.push_back("--keymap");
        tui_argv.push_back(keymap_path);
    }
    std::unique_ptr<sonora::platform::ChildProcess> tui;
    const auto start_tui = [&]() {
        std::string err;
        tui = sonora::platform::ChildProcess::spawn(tui_argv, {}, err);
        if (!tui) std::fprintf(stderr, "sonora: %s\n", err.c_str());
    };
    if (want_tui && have_tui) start_tui();

    int tui_starts = tui ? 1 : 0;
    while (!sonora::quit_requested()) {
        // The backend stopped calling the session. The interface shows why
        // and leaves; the take is closed below and the unit restarts us.
        if (audio->fatal()) {
            if (tui) tui->wait(3000);
            break;
        }
        if (tui && !tui->running()) {
            // Its quit key: the session ends, as it always has on the tty.
            if (tui->exited_normally() && tui->exit_code() == 0) break;
            // A key map it refuses at once is a startup error, as it was
            // when the engine loaded the map itself; restarting cannot fix it.
            if (tui->exited_normally() && tui->exit_code() == 2 && tui_starts == 1) {
                std::fprintf(stderr, "sonora: the terminal interface refused its options\n");
                break;
            }
            std::fprintf(stderr,
                         "sonora: the terminal interface stopped (%s %d); the engine and"
                         " any take carry on, and it is started again\n",
                         tui->exited_normally() ? "exit" : "signal",
                         tui->exited_normally() ? tui->exit_code() : tui->signal_number());
            ::usleep(1000000);
            start_tui();
            ++tui_starts;
        }
        ::usleep(50000);
    }
    if (tui) {
        tui->terminate();
        if (!tui->wait(2000)) tui->kill();
        tui->wait(-1);
    }

    // No command runs while the take closes, and no interface is attached.
    sockets.stop();
    server.stop();
    // stop() says on stderr why a take failed, and the terminal is already
    // the operator's again.
    const bool take_ok = session.stop();
    // To the journal on the appliance: where the sync's cost on the stick
    // is measured while the operator records.
    if (session.disk().checkpoints() > 0) {
        std::printf("sonora: longest checkpoint %.1f ms over %u checkpoint passes\n",
                    static_cast<double>(session.disk().longest_checkpoint_ns()) / 1e6,
                    static_cast<unsigned>(session.disk().checkpoints()));
    }
    sonora::arm_exit_watchdog(kTeardownSeconds, kTeardownHung);
    audio->deactivate();
    server.after_deactivate();
    std::printf("sonora: session written to %s\n", cfg.dir.c_str());

    // The server dying, or changing rate or buffer size, stops the callback
    // without stopping us. The take is closed, so what was captured is a
    // valid file; the service unit restarts us against a fresh server.
    if (audio->fatal()) {
        std::fprintf(stderr, "sonora: %s.\n", audio->fatal_reason().c_str());
        return 1;
    }
    // A quit or SIGTERM in the middle of a failed take is not a clean exit.
    return take_ok ? 0 : 1;
}

