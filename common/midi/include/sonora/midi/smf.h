// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Standard MIDI File reader and writer, per the published MIDI 1.0
// "Standard MIDI Files 1.0" specification (ADR-0004: published formats).
//
// Formats 0 and 1 are read and written. Format 2 (independent sequences)
// is read, so its contents can be inspected or converted, and refused by
// the writer: nothing here produces one.
//
// Chunks other than MThd and MTrk are skipped, as the specification asks,
// and so are bytes after the last announced track.
//
// Parsing works on a byte span, never on a path: opening files belongs to
// whoever calls this (ADR-0008), and a span is what a test can build.
// Nothing throws; every failure comes back as a diagnostic with the byte
// offset where the file stopped making sense.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "sonora/midi/event.h"

namespace sonora::midi {

enum class SmfError : std::uint8_t {
    None,
    TooLarge,             // the file is over SmfReadLimits::max_file_bytes
    Truncated,            // a chunk or an event runs past the end of its data
    NotMidiFile,          // the file does not start with an MThd chunk
    BadChunk,             // a chunk type that is not four ASCII characters
    BadHeaderLength,      // MThd shorter than 6 bytes
    BadFormat,            // format above 2, or format 0 with other than 1 track
    BadDivision,          // zero ticks per quarter, or an unknown SMPTE rate
    TrackCountMismatch,   // fewer MTrk chunks than the header announced
    TooManyTracks,
    TooManyEvents,
    EventTooLarge,        // a SysEx or meta longer than max_event_bytes
    BadVlq,               // a variable-length quantity longer than 4 bytes
    MissingRunningStatus, // a data byte with no status in effect
    BadStatus,            // a status byte that cannot appear in a file
    BadDataByte,          // a channel-message data byte with its top bit set
    BadMetaLength,        // a meta event of a fixed-length type, wrong length
    MissingEndOfTrack,
    DataAfterEndOfTrack,
    TickOutOfRange,       // an absolute time past 2^38 ticks, where TempoMap
                          // arithmetic would overflow
    // Writer only.
    UnsupportedFormat,
    InvalidEvent,
    UnsortedEvents,
    DeltaTooLarge,
};

struct SmfDiagnostic {
    SmfError code = SmfError::None;
    // Reader: offset in the input. Writer: index of the track, and of the
    // event within it, in event_index.
    std::size_t offset = 0;
    std::size_t track = 0;
    std::size_t event_index = 0;
    std::string message;
};

// The header's division word, kept raw so that it round-trips exactly.
struct Division {
    std::uint16_t raw = 960;

    static Division ticks_per_quarter(std::uint16_t ppq) {
        return Division{static_cast<std::uint16_t>(ppq & 0x7FFF)};
    }
    // fps is 24, 25, 29 (30 drop-frame) or 30.
    static Division smpte(int fps, std::uint8_t ticks_per_frame) {
        const auto hi = static_cast<std::uint8_t>(static_cast<std::int8_t>(-fps));
        return Division{static_cast<std::uint16_t>((hi << 8) | ticks_per_frame)};
    }

    bool is_smpte() const { return (raw & 0x8000) != 0; }
    int ppq() const { return is_smpte() ? 0 : raw; }
    int smpte_fps() const {
        return is_smpte() ? -static_cast<int>(static_cast<std::int8_t>(raw >> 8)) : 0;
    }
    int ticks_per_frame() const { return is_smpte() ? (raw & 0xFF) : 0; }

    friend bool operator==(Division a, Division b) { return a.raw == b.raw; }
};

struct SmfTrack {
    // Absolute ticks, in file order, ending with the end of track.
    EventList events;

    friend bool operator==(const SmfTrack& a, const SmfTrack& b) {
        return a.events == b.events;
    }
};

struct Smf {
    std::uint16_t format = 1;
    Division division;
    std::vector<SmfTrack> tracks;

    friend bool operator==(const Smf& a, const Smf& b) {
        return a.format == b.format && a.division == b.division && a.tracks == b.tracks;
    }
};

// Bounds that keep a hostile or corrupt file from exhausting memory. The
// defaults are far above any real arrangement and far below the RAM of
// the target machine.
struct SmfReadLimits {
    std::size_t max_file_bytes = 64u << 20;
    std::size_t max_tracks = 4096;
    std::size_t max_events = 8u << 20;      // over all tracks
    std::size_t max_event_bytes = 4u << 20; // one SysEx or meta payload
};

struct SmfReadResult {
    std::optional<Smf> smf;
    SmfDiagnostic error;

    bool ok() const { return smf.has_value(); }
};

SmfReadResult read_smf(const std::uint8_t* data, std::size_t size,
                       const SmfReadLimits& limits = {});

struct SmfWriteOptions {
    // Omit repeated channel status bytes. Smaller files; every reader must
    // accept it, so it is the default.
    bool running_status = true;
};

struct SmfWriteResult {
    std::vector<std::uint8_t> bytes;
    SmfDiagnostic error;

    bool ok() const { return error.code == SmfError::None; }
};

// Each track must be sorted by tick, start at tick 0 or later, and hold at
// most one end of track, as its last event; one is appended at the last
// tick when it is missing.
SmfWriteResult write_smf(const Smf& smf, const SmfWriteOptions& options = {});

// Variable-length quantities, 7 bits per byte, most significant first.
// Values above 0x0FFFFFFF, the most four bytes hold, are not representable.
constexpr std::uint32_t max_vlq = 0x0FFFFFFF;
void append_vlq(std::vector<std::uint8_t>& out, std::uint32_t value);
// Reads one quantity at data[pos], advancing pos. Empty when it runs past
// size or does not end within 4 bytes; pos is then unspecified.
std::optional<std::uint32_t> read_vlq(const std::uint8_t* data, std::size_t size,
                                      std::size_t& pos);

// Every track's events in one list, for a tempo map or a format 0 export.
// Each track's end of track is dropped and one is put last.
EventList merge_tracks(const Smf& smf);

}  // namespace sonora::midi
