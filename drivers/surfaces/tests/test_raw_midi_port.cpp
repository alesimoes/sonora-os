// SPDX-License-Identifier: GPL-3.0-or-later
// RawMidiPort over a pipe pair standing in for a device node: bytes the
// device sends come out of read(), non-blocking, and write() reaches it. A
// whole MCU poll runs over the pipes.
#include <unistd.h>

#include <cstdio>
#include <string>

#include "sonora/surfaces/mcu.h"

using namespace sonora::surfaces;

namespace {
int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)
}  // namespace

int main() {
    int to_host[2], to_device[2];
    CHECK(::pipe(to_host) == 0);
    CHECK(::pipe(to_device) == 0);
    RawMidiPort port(to_host[0], to_device[1]);

    std::uint8_t buf[16];
    CHECK(port.read(buf, sizeof buf) == 0);  // empty and non-blocking, not a hang

    const std::uint8_t press[] = {0x90, McuProtocol::kNotePlay, 0x7F};
    CHECK(::write(to_host[1], press, 3) == 3);
    CHECK(port.read(buf, sizeof buf) == 3 && buf[1] == McuProtocol::kNotePlay);

    CHECK(::write(to_host[1], press, 3) == 3);
    SurfaceLink link;
    McuProtocol mcu(port, McuProtocol::Config{});
    mcu.poll(link);
    SurfaceEvent e{};
    CHECK(link.pop_event(e) && e.transport == TransportCmd::Play);

    // The first poll drew the page: the device end holds bytes now.
    std::uint8_t sink[4096];
    const ssize_t got = ::read(to_device[0], sink, sizeof sink);
    CHECK(got > 0);
    CHECK(sink[0] == 0xE0 || sink[0] == 0xB0 || sink[0] == 0x90 || sink[0] == 0xF0 ||
          sink[0] == 0xD0);

    std::string error;
    CHECK(RawMidiPort::open("/nonexistent/midi", error) == nullptr);
    CHECK(!error.empty());

    ::close(to_host[1]);
    ::close(to_device[0]);
    if (failures == 0) std::printf("raw midi port: ok\n");
    return failures == 0 ? 0 : 1;
}
