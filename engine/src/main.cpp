// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-engine: the recording core. Phase 1 drives it from a terminal UI so
// the audio path can be proven before any graphical shell exists.

#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <termios.h>
#include <unistd.h>

#include "sonora/jack_backend.h"
#include "sonora/session.h"

namespace {

volatile std::sig_atomic_t g_quit = 0;
void on_signal(int) { g_quit = 1; }

struct TermiosGuard {
    termios saved{};
    bool active = false;

    void raw() {
        if (tcgetattr(STDIN_FILENO, &saved) != 0) return;
        termios t = saved;
        t.c_lflag &= ~(ICANON | ECHO);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0) active = true;
    }
    ~TermiosGuard() {
        if (active) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    }
};

void draw(sonora::Session& s, sonora::JackBackend& jack, int take) {
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
    std::printf("\n  space=rec/stop  1-8=arm  a=arm all  q=quit\n");
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
    while (waited < seconds && !g_quit) {
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

    if (!session.start_recording(1)) {
        std::fprintf(stderr, "sonora: no track could be opened for writing.\n");
        return 1;
    }

    const uint64_t target =
        static_cast<uint64_t>(seconds * session.transport().sample_rate());

    // Wall-clock bound as well as a sample bound: if the server stops calling
    // the callback the playhead stops advancing, and waiting on samples alone
    // would hang forever instead of failing.
    const double deadline = seconds * 2.0 + 5.0;
    double waited = 0.0;

    while (!g_quit && session.transport().playhead() < target) {
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

    session.stop();

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

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            cfg.dir = argv[++i];
        } else if (std::strcmp(argv[i], "--tracks") == 0 && i + 1 < argc) {
            cfg.track_count = std::atoi(argv[++i]);
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
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf(
                "usage: sonora-engine [options]\n"
                "  --dir PATH        session directory\n"
                "  --tracks N        track count (default 8)\n"
                "  --rate HZ         session sample rate (default 48000)\n"
                "  --float           record 32-bit float instead of 24-bit\n"
                "  --arm-all         arm every track at startup\n"
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

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    if (arm_all) {
        for (const auto& tr : session.tracks()) tr->set_rec_armed(true);
    }

    if (record_seconds > 0.0) {
        const int rc = run_headless(session, jack, record_seconds,
                                    wait_for_inputs_seconds);
        jack.close();
        return rc;
    }

    TermiosGuard term;
    term.raw();

    int take = 1;
    bool server_lost = false;
    while (!g_quit) {
        // The server dying stops the callback without stopping us. Close
        // the take so what was captured is a valid file, then let the
        // service unit restart us against a fresh server.
        if (jack.server_gone()) {
            server_lost = true;
            break;
        }

        char c = 0;
        if (::read(STDIN_FILENO, &c, 1) == 1) {
            if (c == 'q') break;
            if (c == ' ') {
                if (session.transport().state() ==
                    sonora::TransportState::Recording) {
                    session.stop();
                    session.disk().start();  // ready for the next take
                    ++take;
                } else {
                    session.start_recording(take);
                }
            } else if (c == 'a') {
                for (const auto& tr : session.tracks()) tr->set_rec_armed(true);
            } else if (c >= '1' && c <= '8') {
                const std::size_t idx = static_cast<std::size_t>(c - '1');
                if (idx < session.tracks().size()) {
                    auto& tr = session.tracks()[idx];
                    tr->set_rec_armed(!tr->rec_armed());
                }
            }
        }
        draw(session, jack, take);
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
