// SPDX-License-Identifier: GPL-3.0-or-later
// The surface's own byte transport (ADR-0055). A control surface is not a
// MIDI instrument: nothing here reaches the engine's MIDI model. A port moves
// raw bytes to and from a device; the protocol above it frames them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace sonora::surfaces {

class MidiPort {
public:
    virtual ~MidiPort() = default;
    // Non-blocking. Copies up to `cap` pending bytes, returns how many; 0
    // when there is nothing. Chunks may split a message: the protocol keeps
    // its own parser state.
    virtual std::size_t read(std::uint8_t* buf, std::size_t cap) = 0;
    // One complete message (or SysEx frame). False when the device did not
    // take all of it; the caller drops it, a motor fader is state and the
    // next diff resends it.
    virtual bool write(const std::uint8_t* bytes, std::size_t n) = 0;
};

// A raw MIDI device node (/dev/snd/midiC1D0) or any pair of descriptors, for
// tests a pipe. Linux only; the file is in drivers/surfaces/src.
class RawMidiPort : public MidiPort {
public:
    // Opens `path` read/write, non-blocking. Null with `error` set on failure.
    static RawMidiPort* open(const std::string& path, std::string& error);
    // Takes ownership of both descriptors (they may be the same).
    RawMidiPort(int read_fd, int write_fd);
    ~RawMidiPort() override;
    RawMidiPort(const RawMidiPort&) = delete;
    RawMidiPort& operator=(const RawMidiPort&) = delete;

    std::size_t read(std::uint8_t* buf, std::size_t cap) override;
    bool write(const std::uint8_t* bytes, std::size_t n) override;

private:
    int read_fd_;
    int write_fd_;
};

}  // namespace sonora::surfaces
