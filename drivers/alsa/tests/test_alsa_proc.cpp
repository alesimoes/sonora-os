// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>

#include "sonora/alsa_proc.h"

namespace {

int failed = 0;

void expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failed;
    }
}

// Shaped after sound/usb/proc.c's output; the UMC1820's own file has not
// been captured yet, so the numbers are the ones its datasheet states.
const char* kStream0 =
    "Behringer UMC1820 at usb-0000:00:14.0-2, high speed : USB Audio\n"
    "\n"
    "Playback:\n"
    "  Status: Stop\n"
    "  Interface 1\n"
    "    Altset 1\n"
    "    Format: S24_3LE\n"
    "    Channels: 20\n"
    "    Endpoint: 0x01 (1 OUT) (ASYNC)\n"
    "    Rates: 44100, 48000, 88200, 96000\n"
    "    Bits: 24\n"
    "  Interface 1\n"
    "    Altset 2\n"
    "    Format: S24_3LE\n"
    "    Channels: 12\n"
    "    Rates: 44100, 48000\n"
    "\n"
    "Capture:\n"
    "  Status: Stop\n"
    "  Interface 2\n"
    "    Altset 1\n"
    "    Format: S24_3LE\n"
    "    Channels: 18\n"
    "    Rates: 44100, 48000, 88200, 96000\n";

void test_stream0() {
    const auto s = sonora::alsa::parse_stream0(kStream0);
    expect(s.playback_channels == 20, "the largest playback altsetting is 20 channels");
    expect(s.capture_channels == 18, "the capture stream is 18 channels");
    expect(s.rates.size() == 4 && s.rates.front() == 44100 && s.rates.back() == 96000,
           "rates are the sorted union");
    const auto none = sonora::alsa::parse_stream0("");
    expect(none.capture_channels == 0 && none.playback_channels == 0, "empty text is no stream");
    const auto range = sonora::alsa::parse_stream0(
        "Capture:\n  Interface 1\n    Channels: 2\n    Rates: 8000 - 48000 (continuous)\n");
    expect(range.capture_channels == 2 && range.rates.empty(),
           "a continuous range means any rate, not a list");
}

void test_resolve() {
    using sonora::alsa::CardInfo;
    CardInfo umc;
    umc.index = 1;
    umc.id = "UMC1820";
    umc.name = "Behringer UMC1820";
    CardInfo pch;
    pch.index = 0;
    pch.id = "PCH";
    pch.name = "HDA Intel PCH";
    const std::vector<CardInfo> cards{pch, umc};
    using sonora::alsa::resolve_card;
    expect(resolve_card(cards, "UMC1820") == "UMC1820", "by id");
    expect(resolve_card(cards, "umc1820") == "UMC1820", "case does not matter");
    expect(resolve_card(cards, "Behringer UMC1820") == "UMC1820", "by name");
    expect(resolve_card(cards, "hw:UMC1820") == "UMC1820", "hw:ID");
    expect(resolve_card(cards, "hw:CARD=UMC1820,DEV=0") == "UMC1820", "hw:CARD=ID,DEV=0");
    expect(resolve_card(cards, "hw:1,0") == "UMC1820", "hw:N,0");
    expect(resolve_card(cards, "1") == "UMC1820", "index");
    expect(resolve_card(cards, "UMC") == "", "a prefix is not a match");
    expect(resolve_card(cards, "") == "", "empty names nothing");
}

}  // namespace

int main() {
    test_stream0();
    test_resolve();
    if (failed == 0) std::printf("alsa_proc: all passed\n");
    return failed == 0 ? 0 : 1;
}
