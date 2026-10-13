// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/midi/stream_parser.h"

#include <vector>

#include "check.h"

using namespace sonora::midi;
using Result = StreamParser::Result;

namespace {

struct Collected {
    std::vector<WireMessage> messages;
    std::vector<std::vector<std::uint8_t>> sysex;
};

Collected feed_all(StreamParser& p, const std::vector<std::uint8_t>& bytes) {
    Collected c;
    for (const std::uint8_t b : bytes) {
        switch (p.feed(b)) {
            case Result::Message: c.messages.push_back(p.message()); break;
            case Result::SysEx: c.sysex.push_back(p.sysex()); break;
            case Result::None: break;
        }
    }
    return c;
}

bool is(const WireMessage& m, std::vector<std::uint8_t> bytes) {
    if (m.size != bytes.size()) return false;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (m.bytes[i] != bytes[i]) return false;
    }
    return true;
}

int running_status_is_expanded() {
    StreamParser p;
    const Collected c = feed_all(p, {0x90, 60, 100, 62, 100, 60, 0, 0xC1, 5, 6});
    CHECK(c.messages.size() == 5);
    CHECK(is(c.messages[0], {0x90, 60, 100}));
    CHECK(is(c.messages[1], {0x90, 62, 100}));
    CHECK(is(c.messages[2], {0x90, 60, 0}));
    CHECK(is(c.messages[3], {0xC1, 5}));
    CHECK(is(c.messages[4], {0xC1, 6}));  // running status on a 1-byte message
    CHECK(p.errors() == 0);

    // A note-on of 0 through running status becomes a note-off event.
    const auto e = to_event(c.messages[2], 42);
    CHECK(e && e->tick == 42 && e->is_note_off());
    CHECK(to_event(c.messages[3], 0)->kind() == Kind::ProgramChange);
    return 0;
}

int real_time_bytes_interleave_anywhere() {
    StreamParser p;
    // Clock between status and data, and between the two data bytes.
    const Collected c = feed_all(p, {0x90, 0xF8, 60, 0xFE, 100, 62, 0xFA, 101});
    CHECK(c.messages.size() == 5);
    CHECK(is(c.messages[0], {0xF8}));
    CHECK(is(c.messages[1], {0xFE}));
    CHECK(is(c.messages[2], {0x90, 60, 100}));
    CHECK(is(c.messages[3], {0xFA}));
    CHECK(is(c.messages[4], {0x90, 62, 101}));  // running status survived
    CHECK(!to_event(c.messages[0], 0));
    return 0;
}

int sysex_is_collected_whole() {
    StreamParser p;
    const Collected c = feed_all(p, {0x90, 60, 1, 0xF0, 0x7E, 0xF8, 0x7F, 0x09, 0x01, 0xF7,
                                     61, 1});
    CHECK(c.sysex.size() == 1);
    CHECK(c.sysex[0] == std::vector<std::uint8_t>({0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7}));
    // The clock inside the SysEx came out on its own.
    CHECK(c.messages.size() == 2);
    CHECK(is(c.messages[1], {0xF8}));
    // SysEx cancels running status: the trailing data bytes are dropped.
    CHECK(p.errors() == 2);
    return 0;
}

int interrupted_and_oversized_sysex() {
    StreamParser p;
    Collected c = feed_all(p, {0xF0, 0x41, 0x10, 0x90, 60, 100});
    CHECK(c.sysex.empty());
    CHECK(c.messages.size() == 1 && is(c.messages[0], {0x90, 60, 100}));
    CHECK(p.errors() == 1);

    StreamParser small(4);
    c = feed_all(small, {0xF0, 1, 2, 3, 4, 5, 0xF7, 0xF0, 1, 2, 0xF7});
    CHECK(c.sysex.size() == 1);
    CHECK(c.sysex[0] == std::vector<std::uint8_t>({0xF0, 1, 2, 0xF7}));
    CHECK(small.errors() == 1);
    return 0;
}

int system_common_and_strays() {
    StreamParser p;
    // Song position pointer, then data with no running status.
    const Collected c = feed_all(p, {0xF2, 0x10, 0x20, 0x30, 0xF3, 7, 0xF6, 0xF7, 0xF4});
    CHECK(c.messages.size() == 3);
    CHECK(is(c.messages[0], {0xF2, 0x10, 0x20}));
    CHECK(is(c.messages[1], {0xF3, 7}));
    CHECK(is(c.messages[2], {0xF6}));
    CHECK(!to_event(c.messages[0], 0));
    // The stray 0x30, the stray 0xF7, the undefined 0xF4.
    CHECK(p.errors() == 3);

    // A message cut short by a new status is counted and dropped.
    StreamParser q;
    const Collected d = feed_all(q, {0x90, 60, 0x80, 60, 0});
    CHECK(d.messages.size() == 1 && is(d.messages[0], {0x80, 60, 0}));
    CHECK(q.errors() == 1);
    return 0;
}

}  // namespace

int main() {
    RUN(running_status_is_expanded);
    RUN(real_time_bytes_interleave_anywhere);
    RUN(sysex_is_collected_whole);
    RUN(interrupted_and_oversized_sysex);
    RUN(system_common_and_strays);
    std::printf("midi_stream_parser: all tests passed\n");
    return 0;
}
