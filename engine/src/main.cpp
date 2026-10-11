// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-engine: the recording core. Phase 1 drives it from a terminal UI so
// the audio path can be proven before any graphical shell exists.

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

}  // namespace

int main(int argc, char** argv) {
    sonora::SessionConfig cfg;
    cfg.dir = std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") +
              "/Sessions/Untitled";

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            cfg.dir = argv[++i];
        } else if (std::strcmp(argv[i], "--tracks") == 0 && i + 1 < argc) {
            cfg.track_count = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            cfg.sample_rate = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--float") == 0) {
            cfg.format = sonora::WavWriter::Format::Float32;
        }
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
    jack.auto_connect_inputs();
    session.disk().start();

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    TermiosGuard term;
    term.raw();

    int take = 1;
    while (!g_quit) {
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
    return 0;
}
