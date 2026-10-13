// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-fake-capture N [CLIENT]: a JACK client that registers N physical
// capture ports and holds them until killed. tools/test-engine-entrypoint.sh
// starts it with PIPEWIRE_PROPS setting an onboard card's object.path, so the
// container has the non-USB device a laptop has: left alone while something
// is connected, recorded from after the wait when it is the only device, and
// killed mid-take to stand in for an unplug. A second one, under another
// CLIENT name, makes the device ambiguous. Not shipped.
#include <jack/jack.h>

#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

std::vector<jack_port_t*> g_ports;
volatile std::sig_atomic_t g_stop = 0;

int process(jack_nframes_t nframes, void*) {
    for (jack_port_t* p : g_ports) {
        if (auto* buf = static_cast<float*>(jack_port_get_buffer(p, nframes))) {
            std::memset(buf, 0, nframes * sizeof(float));
        }
    }
    return 0;
}

void on_signal(int) { g_stop = 1; }

}  // namespace

int main(int argc, char** argv) {
    const int count = argc > 1 ? std::atoi(argv[1]) : 2;
    jack_status_t status{};
    const char* name = argc > 2 ? argv[2] : "fake-capture";
    jack_client_t* client = jack_client_open(name, JackNoStartServer, &status);
    if (!client) {
        std::fprintf(stderr, "sonora-fake-capture: no server\n");
        return 1;
    }
    for (int i = 0; i < count; ++i) {
        char port[32];
        std::snprintf(port, sizeof port, "capture_AUX%d", i);
        jack_port_t* p = jack_port_register(
            client, port, JACK_DEFAULT_AUDIO_TYPE,
            JackPortIsOutput | JackPortIsPhysical | JackPortIsTerminal, 0);
        if (!p) {
            std::fprintf(stderr, "sonora-fake-capture: cannot register %s\n", port);
            return 1;
        }
        g_ports.push_back(p);
    }
    jack_set_process_callback(client, process, nullptr);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT, on_signal);
    if (jack_activate(client) != 0) return 1;
    while (!g_stop) ::pause();
    jack_client_close(client);
    return 0;
}
