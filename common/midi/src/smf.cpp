// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/midi/smf.h"

#include <cstring>
#include <utility>

#include "sonora/midi/notes.h"

namespace sonora::midi {

namespace {

std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
           (std::uint32_t{p[2]} << 8) | std::uint32_t{p[3]};
}

std::uint16_t be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void put16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

std::size_t data_bytes(std::uint8_t status) {
    const std::uint8_t hi = status & 0xF0;
    return (hi == status::program_change || hi == status::channel_aftertouch) ? 1 : 2;
}

// The meta types whose length the specification fixes. A wrong length
// there means the parser and the file disagree about where the next event
// starts, so it is an error rather than something to skip.
bool meta_length_ok(std::uint8_t type, std::size_t len) {
    switch (type) {
        case meta::sequence_number: return len == 0 || len == 2;
        case meta::channel_prefix: return len == 1;
        case meta::end_of_track: return len == 0;
        case meta::tempo: return len == 3;
        case meta::smpte_offset: return len == 5;
        case meta::time_signature: return len == 4;
        case meta::key_signature: return len == 2;
        default: return true;
    }
}

bool division_ok(Division d) {
    if (!d.is_smpte()) return d.raw != 0;
    const int fps = d.smpte_fps();
    return (fps == 24 || fps == 25 || fps == 29 || fps == 30) && d.ticks_per_frame() != 0;
}

std::string hex(std::size_t v) {
    static const char digits[] = "0123456789abcdef";
    std::string s;
    do {
        s.insert(s.begin(), digits[v & 0xF]);
        v >>= 4;
    } while (v != 0);
    return "0x" + s;
}

enum class VlqStatus { Ok, Truncated, TooLong };

VlqStatus read_vlq_at(const std::uint8_t* data, std::size_t end, std::size_t& pos,
                      std::uint32_t& value) {
    value = 0;
    for (int i = 0; i < 4; ++i) {
        if (pos >= end) return VlqStatus::Truncated;
        const std::uint8_t b = data[pos++];
        value = (value << 7) | (b & 0x7F);
        if ((b & 0x80) == 0) return VlqStatus::Ok;
    }
    return VlqStatus::TooLong;
}

class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size, const SmfReadLimits& limits)
        : data_(data), size_(size), limits_(limits) {}

    SmfReadResult run();

private:
    bool fail(SmfError code, std::size_t offset, std::string message) {
        error_.code = code;
        error_.offset = offset;
        error_.message = std::move(message) + " (at byte " + std::to_string(offset) +
                         ", " + hex(offset) + ")";
        return false;
    }

    bool read_track(std::size_t begin, std::size_t end, SmfTrack& track);
    bool read_length(std::size_t end, std::size_t& pos, std::uint32_t& value);

    const std::uint8_t* data_;
    std::size_t size_;
    const SmfReadLimits& limits_;
    std::size_t total_events_ = 0;
    SmfDiagnostic error_;
};

bool Reader::read_length(std::size_t end, std::size_t& pos, std::uint32_t& value) {
    const std::size_t at = pos;
    switch (read_vlq_at(data_, end, pos, value)) {
        case VlqStatus::Ok: return true;
        case VlqStatus::Truncated:
            return fail(SmfError::Truncated, at,
                        "variable-length quantity runs past the end of the track");
        case VlqStatus::TooLong:
            return fail(SmfError::BadVlq, at,
                        "variable-length quantity longer than 4 bytes");
    }
    return false;
}

bool Reader::read_track(std::size_t begin, std::size_t end, SmfTrack& track) {
    Tick tick = 0;
    std::uint8_t running = 0;
    bool ended = false;
    std::size_t pos = begin;

    while (pos < end) {
        if (ended) {
            return fail(SmfError::DataAfterEndOfTrack, pos,
                        "bytes after the end-of-track event");
        }
        if (++total_events_ > limits_.max_events) {
            return fail(SmfError::TooManyEvents, pos,
                        "more than " + std::to_string(limits_.max_events) + " events");
        }
        std::uint32_t delta = 0;
        if (!read_length(end, pos, delta)) return false;
        tick += delta;
        // TempoMap's tick-to-sample products are exact only below 2^38 ticks;
        // past that a hostile file would overflow them (signed, so UB).
        if (tick > (std::int64_t{1} << 38)) {
            return fail(SmfError::TickOutOfRange, pos,
                        "event time past 2^38 ticks");
        }
        if (pos >= end) {
            return fail(SmfError::Truncated, pos, "track ends after a delta time");
        }

        const std::size_t status_at = pos;
        std::uint8_t st = data_[pos];
        if (st < 0x80) {
            if (running == 0) {
                return fail(SmfError::MissingRunningStatus, pos,
                            "data byte with no running status in effect");
            }
            st = running;
        } else {
            ++pos;
        }

        Event e;
        e.tick = tick;
        e.status = st;

        if (st < 0xF0) {
            running = st;
            const std::size_t n = data_bytes(st);
            if (end - pos < n) {
                return fail(SmfError::Truncated, pos, "channel message cut short");
            }
            for (std::size_t i = 0; i < n; ++i) {
                if (data_[pos + i] >= 0x80) {
                    return fail(SmfError::BadDataByte, pos + i,
                                "channel message data byte has its top bit set");
                }
            }
            e.data1 = data_[pos];
            e.data2 = n == 2 ? data_[pos + 1] : 0;
            pos += n;
        } else if (st == status::sysex || st == status::sysex_escape ||
                   st == status::meta) {
            // SysEx and meta events cancel running status (SMF 1.0).
            running = 0;
            if (st == status::meta) {
                if (pos >= end) {
                    return fail(SmfError::Truncated, pos, "meta event cut short");
                }
                e.data1 = data_[pos];
                if (e.data1 >= 0x80) {
                    return fail(SmfError::BadDataByte, pos,
                                "meta event type has its top bit set");
                }
                ++pos;
            }
            const std::size_t length_at = pos;
            std::uint32_t len = 0;
            if (!read_length(end, pos, len)) return false;
            if (len > limits_.max_event_bytes) {
                return fail(SmfError::EventTooLarge, length_at,
                            "event of " + std::to_string(len) + " bytes exceeds the limit of " +
                                std::to_string(limits_.max_event_bytes));
            }
            if (len > end - pos) {
                return fail(SmfError::Truncated, length_at,
                            "event of " + std::to_string(len) +
                                " bytes runs past the end of the track");
            }
            if (st == status::meta && !meta_length_ok(e.data1, len)) {
                return fail(SmfError::BadMetaLength, status_at,
                            "meta event " + hex(e.data1) + " has length " +
                                std::to_string(len));
            }
            e.payload.assign(data_ + pos, data_ + pos + len);
            pos += len;
            if (e.is_end_of_track()) ended = true;
        } else {
            return fail(SmfError::BadStatus, status_at,
                        "status byte " + hex(st) + " cannot appear in a MIDI file");
        }
        track.events.push_back(std::move(e));
    }
    if (!ended) {
        return fail(SmfError::MissingEndOfTrack, end, "track has no end-of-track event");
    }
    return true;
}

SmfReadResult Reader::run() {
    SmfReadResult result;
    auto failed = [&]() {
        result.smf.reset();
        result.error = error_;
        return result;
    };

    if (size_ > limits_.max_file_bytes) {
        fail(SmfError::TooLarge, 0,
             "file of " + std::to_string(size_) + " bytes exceeds the limit of " +
                 std::to_string(limits_.max_file_bytes));
        return failed();
    }
    if (size_ < 4 || std::memcmp(data_, "MThd", 4) != 0) {
        fail(SmfError::NotMidiFile, 0, "no MThd chunk at the start");
        return failed();
    }
    if (size_ < 8) {
        fail(SmfError::Truncated, 4, "MThd chunk has no length");
        return failed();
    }
    const std::uint32_t header_len = be32(data_ + 4);
    if (header_len < 6) {
        fail(SmfError::BadHeaderLength, 4,
             "MThd length " + std::to_string(header_len) + " is below 6");
        return failed();
    }
    if (header_len > size_ - 8) {
        fail(SmfError::Truncated, 4, "MThd chunk runs past the end of the file");
        return failed();
    }

    Smf smf;
    smf.format = be16(data_ + 8);
    const std::uint16_t ntracks = be16(data_ + 10);
    smf.division.raw = be16(data_ + 12);
    if (smf.format > 2) {
        fail(SmfError::BadFormat, 8, "format " + std::to_string(smf.format) + " is not 0, 1 or 2");
        return failed();
    }
    if (smf.format == 0 && ntracks != 1) {
        fail(SmfError::BadFormat, 10,
             "format 0 file declares " + std::to_string(ntracks) + " tracks, not 1");
        return failed();
    }
    if (ntracks > limits_.max_tracks) {
        fail(SmfError::TooManyTracks, 10,
             std::to_string(ntracks) + " tracks exceed the limit of " +
                 std::to_string(limits_.max_tracks));
        return failed();
    }
    if (!division_ok(smf.division)) {
        fail(SmfError::BadDivision, 12, "invalid division " + hex(smf.division.raw));
        return failed();
    }

    std::size_t pos = 8 + std::size_t{header_len};
    smf.tracks.reserve(ntracks);
    while (smf.tracks.size() < ntracks) {
        if (pos == size_) {
            fail(SmfError::TrackCountMismatch, pos,
                 "header declares " + std::to_string(ntracks) + " tracks, file holds " +
                     std::to_string(smf.tracks.size()));
            return failed();
        }
        if (size_ - pos < 8) {
            fail(SmfError::Truncated, pos, "chunk header cut short");
            return failed();
        }
        for (std::size_t i = 0; i < 4; ++i) {
            const std::uint8_t c = data_[pos + i];
            if (c < 0x20 || c > 0x7E) {
                fail(SmfError::BadChunk, pos, "chunk type is not four ASCII characters");
                return failed();
            }
        }
        const std::uint32_t len = be32(data_ + pos + 4);
        if (len > size_ - pos - 8) {
            fail(SmfError::Truncated, pos + 4,
                 "chunk of " + std::to_string(len) + " bytes runs past the end of the file");
            return failed();
        }
        if (std::memcmp(data_ + pos, "MTrk", 4) == 0) {
            SmfTrack track;
            if (!read_track(pos + 8, pos + 8 + len, track)) return failed();
            smf.tracks.push_back(std::move(track));
        }
        pos += 8 + std::size_t{len};
    }
    result.smf = std::move(smf);
    return result;
}

bool valid_event(const Event& e) {
    switch (e.kind()) {
        case Kind::NoteOff:
        case Kind::NoteOn:
        case Kind::PolyAftertouch:
        case Kind::ControlChange:
        case Kind::ProgramChange:
        case Kind::ChannelAftertouch:
        case Kind::PitchBend:
            return e.data1 < 0x80 && e.data2 < 0x80;
        case Kind::SysEx:
        case Kind::SysExEscape:
            return e.payload.size() <= max_vlq;
        case Kind::Meta:
            return e.data1 < 0x80 && e.payload.size() <= max_vlq &&
                   meta_length_ok(e.data1, e.payload.size());
        case Kind::Invalid:
            break;
    }
    return false;
}

}  // namespace

void append_vlq(std::vector<std::uint8_t>& out, std::uint32_t value) {
    if (value > max_vlq) value = max_vlq;
    std::uint8_t buf[4];
    int n = 0;
    do {
        buf[n++] = static_cast<std::uint8_t>(value & 0x7F);
        value >>= 7;
    } while (value != 0);
    while (n > 1) out.push_back(static_cast<std::uint8_t>(buf[--n] | 0x80));
    out.push_back(buf[0]);
}

std::optional<std::uint32_t> read_vlq(const std::uint8_t* data, std::size_t size,
                                      std::size_t& pos) {
    std::uint32_t value = 0;
    if (read_vlq_at(data, size, pos, value) != VlqStatus::Ok) return std::nullopt;
    return value;
}

SmfReadResult read_smf(const std::uint8_t* data, std::size_t size,
                       const SmfReadLimits& limits) {
    return Reader(data, size, limits).run();
}

SmfWriteResult write_smf(const Smf& smf, const SmfWriteOptions& options) {
    SmfWriteResult result;
    auto fail = [&](SmfError code, std::size_t track, std::size_t index,
                    std::string message) {
        result.bytes.clear();
        result.error.code = code;
        result.error.track = track;
        result.error.event_index = index;
        result.error.message = std::move(message);
        return result;
    };

    if (smf.format == 2) {
        return fail(SmfError::UnsupportedFormat, 0, 0,
                    "format 2 is read-only: it cannot be written");
    }
    if (smf.format > 2) {
        return fail(SmfError::BadFormat, 0, 0,
                    "format " + std::to_string(smf.format) + " is not 0 or 1");
    }
    if (smf.format == 0 && smf.tracks.size() != 1) {
        return fail(SmfError::BadFormat, 0, 0,
                    "format 0 holds exactly one track, not " +
                        std::to_string(smf.tracks.size()));
    }
    if (smf.tracks.size() > 0xFFFF) {
        return fail(SmfError::TooManyTracks, 0, 0, "more than 65535 tracks");
    }
    if (!division_ok(smf.division)) {
        return fail(SmfError::BadDivision, 0, 0, "invalid division " + hex(smf.division.raw));
    }

    std::vector<std::uint8_t>& out = result.bytes;
    out.insert(out.end(), {'M', 'T', 'h', 'd'});
    put32(out, 6);
    put16(out, smf.format);
    put16(out, static_cast<std::uint16_t>(smf.tracks.size()));
    put16(out, smf.division.raw);

    std::vector<std::uint8_t> body;
    for (std::size_t t = 0; t < smf.tracks.size(); ++t) {
        const EventList& events = smf.tracks[t].events;
        body.clear();
        Tick prev = 0;
        std::uint8_t running = 0;
        bool ended = false;

        for (std::size_t i = 0; i < events.size(); ++i) {
            const Event& e = events[i];
            if (ended) {
                return fail(SmfError::InvalidEvent, t, i,
                            "event after the end of track");
            }
            if (e.tick < prev) {
                return fail(SmfError::UnsortedEvents, t, i,
                            "tick " + std::to_string(e.tick) + " comes before tick " +
                                std::to_string(prev));
            }
            if (e.tick - prev > static_cast<Tick>(max_vlq)) {
                return fail(SmfError::DeltaTooLarge, t, i,
                            "gap of " + std::to_string(e.tick - prev) +
                                " ticks does not fit a delta time");
            }
            if (!valid_event(e)) {
                return fail(SmfError::InvalidEvent, t, i,
                            "event with status " + hex(e.status) + " is not valid");
            }
            append_vlq(body, static_cast<std::uint32_t>(e.tick - prev));
            prev = e.tick;

            if (e.is_channel()) {
                if (!options.running_status || e.status != running) {
                    body.push_back(e.status);
                }
                running = e.status;
                body.push_back(e.data1);
                if (data_bytes(e.status) == 2) body.push_back(e.data2);
                continue;
            }
            running = 0;
            body.push_back(e.status);
            if (e.status == status::meta) body.push_back(e.data1);
            append_vlq(body, static_cast<std::uint32_t>(e.payload.size()));
            body.insert(body.end(), e.payload.begin(), e.payload.end());
            if (e.is_end_of_track()) ended = true;
        }
        if (!ended) body.insert(body.end(), {0x00, status::meta, meta::end_of_track, 0x00});
        if (body.size() > 0xFFFFFFFFu) {
            return fail(SmfError::InvalidEvent, t, events.size(),
                        "track larger than an MTrk chunk can hold");
        }

        out.insert(out.end(), {'M', 'T', 'r', 'k'});
        put32(out, static_cast<std::uint32_t>(body.size()));
        out.insert(out.end(), body.begin(), body.end());
    }
    return result;
}

EventList merge_tracks(const Smf& smf) {
    EventList merged;
    for (const SmfTrack& track : smf.tracks) merged = merge_events(merged, track.events);
    return merged;
}

}  // namespace sonora::midi
