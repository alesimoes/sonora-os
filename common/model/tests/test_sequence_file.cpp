// SPDX-License-Identifier: GPL-3.0-or-later
//
// The .msq encoding (ADR-0015 §3, midi.md §4.4): its bytes, defined to the
// byte; a decoder that is bounded, never throws and accepts only what it
// would write; a fuzz sweep; and the saver's bookkeeping (digests, the
// sequences a snapshot names, which files may be deleted).
#include <cstdint>
#include <set>

#include "model_test.h"
#include "sonora/model/sequence_file.h"

using namespace model_test;
namespace midi = sonora::midi;

namespace {

midi::Note note(Ticks start, Ticks length, std::uint8_t channel, std::uint8_t pitch,
                std::uint8_t velocity, std::uint8_t release) {
    midi::Note n;
    n.start = start;
    n.length = length;
    n.channel = channel;
    n.pitch = pitch;
    n.velocity = velocity;
    n.release_velocity = release;
    return n;
}

// The fixture's sequence, as midi.md §4.4 shows it.
MidiSequence example() {
    MidiSequence s;
    s.notes = {note(0, 480000, 0, 36, 96, 64), note(960000, 455000, 1, 43, 88, 0)};
    s.events = {MidiEvent{0, 0xB0, 0, 1}, MidiEvent{0, 0xC0, 5, 0}, MidiEvent{96000, 0xE0, 0, 72}};
    s.sysex = {MidiSysEx{480000, {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7}}};
    return s;
}

std::string from_hex(const std::string& hex) {
    std::string out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

// Computed by an independent encoder written from midi.md §4.4 (Python).
const char* const kExampleHex =
    "534d5351010203010080a61d0024604080cc3ad8e21b012b580000b0000100c0050080ee05e0004880a61d"
    "06f07e7f0901f7";
const char* const kExampleSha = "2b999da68a3660c556eecede2fb2733dd9953c5364b81b7d9e4654ef40e1203c";

bool refused(const std::string& bytes, const char* why) {
    MidiSequence s;
    std::string error;
    if (decode_sequence(bytes, s, error)) {
        std::fprintf(stderr, "  decoded, expected \"%s\"\n", why);
        return false;
    }
    return mentions(error, why);
}

int test_bytes_are_the_spec() {
    const std::string bytes = encode_sequence(example());
    CHECK(bytes == from_hex(kExampleHex));
    MidiSequence back;
    std::string error;
    CHECK(decode_sequence(bytes, back, error));
    CHECK(back == example());
    const SequenceFileRef ref = sequence_ref(example());
    CHECK(ref.sha256 == kExampleSha);
    CHECK(ref.file == std::string("MIDI Sequences/") + kExampleSha + ".msq");
    CHECK(ref.bytes == 50 && ref.notes == 2 && ref.events == 3 && ref.sysex == 1);
    // The empty sequence is a header and three zero counts.
    CHECK(encode_sequence(MidiSequence{}) == from_hex("534d535101000000"));
    return 0;
}

// The edges of LEB128 and of the integer range: a tick of 2^53 - 1 is
// eight bytes, and one past it is refused.
int test_largest_values() {
    MidiSequence s;
    s.events = {MidiEvent{0, 0xB0, 1, 2}, MidiEvent{kMaxInteger, 0xB0, 1, 3}};
    const std::string bytes = encode_sequence(s);
    MidiSequence back;
    std::string error;
    CHECK(decode_sequence(bytes, back, error));
    CHECK(back == s);
    // The second event's delta, 2^53 - 1, as eight bytes; bump its top group
    // to 2^53.
    std::string over = bytes;
    const std::size_t top = over.size() - 4;   // its last LEB byte, before the 3 data bytes
    CHECK(static_cast<unsigned char>(over[top]) == 0x0F);
    over[top] = 0x10;
    CHECK(refused(over, "past 2^53 - 1"));
    return 0;
}

int test_refusals() {
    const std::string good = from_hex(kExampleHex);
    CHECK(refused("", "magic"));
    CHECK(refused("SMS", "magic"));
    CHECK(refused("XMSQ\x01" + good.substr(5), "magic"));
    std::string v2 = good;
    v2[4] = 2;
    CHECK(refused(v2, "version 2"));
    // Trailing bytes, and every truncation.
    CHECK(refused(good + std::string(1, '\0'), "bytes after the last entry"));
    for (std::size_t n = 5; n < good.size(); ++n) {
        MidiSequence s;
        std::string error;
        CHECK(!decode_sequence(good.substr(0, n), s, error));
    }
    // A count the bytes cannot hold is refused before anything is reserved.
    CHECK(refused(from_hex("534d535101ffffffffffffff0f0000"), "counts promise more"));
    // A non-minimal LEB128: the note count 2 spelt 0x82 0x00.
    CHECK(refused(from_hex("534d5351018200") + good.substr(6), "not minimal"));
    // Out of order: a delta can only go forward, so the order breaks on the
    // channel of two notes at one tick.
    MidiSequence unsorted;
    unsorted.notes = {note(0, 1, 1, 60, 100, 0), note(0, 1, 0, 60, 100, 0)};
    CHECK(refused(encode_sequence(unsorted), "notes are not in order"));
    // A value out of range: velocity 0.
    MidiSequence bad;
    bad.notes = {note(0, 1, 0, 60, 0, 0)};
    CHECK(refused(encode_sequence(bad), "velocity"));
    // A SysEx over 4,096 bytes.
    MidiSequence big;
    big.sysex = {MidiSysEx{0, std::vector<std::uint8_t>(kMaxSysexBytes + 1, 0x01)}};
    big.sysex[0].bytes.front() = 0xF0;
    big.sysex[0].bytes.back() = 0xF7;
    CHECK(refused(encode_sequence(big), "at most 4096 bytes"));
    return 0;
}

// Whatever the bytes, the decoder returns; and what it accepts is canonical:
// it re-encodes to exactly the bytes it read.
int test_fuzz() {
    std::uint64_t x = 0x2545F4914F6CDD1Dull;
    const auto next = [&] {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        return x;
    };
    MidiSequence dense = example();
    for (int i = 0; i < 200; ++i) {
        dense.events.push_back(MidiEvent{96000 + i * 1000, 0xB1, 7, static_cast<std::uint8_t>(i % 128)});
    }
    const std::string seeds[] = {from_hex(kExampleHex), encode_sequence(dense),
                                 encode_sequence(MidiSequence{})};
    int accepted = 0;
    for (int round = 0; round < 20000; ++round) {
        std::string bytes = seeds[round % 3];
        const int edits = 1 + static_cast<int>(next() % 4);
        for (int e = 0; e < edits && !bytes.empty(); ++e) {
            const std::size_t at = next() % bytes.size();
            switch (next() % 4) {
                case 0: bytes[at] = static_cast<char>(next()); break;
                case 1: bytes[at] = static_cast<char>(bytes[at] ^ (1 << (next() % 8))); break;
                case 2: bytes.erase(at, 1 + next() % 3); break;
                default: bytes.insert(at, 1, static_cast<char>(next())); break;
            }
        }
        MidiSequence s;
        std::string error;
        if (!decode_sequence(bytes, s, error)) {
            CHECK(!error.empty());
            continue;
        }
        ++accepted;
        CHECK(encode_sequence(s) == bytes);
    }
    // Some mutations (a data byte changed in range) still decode.
    CHECK(accepted > 0);
    return 0;
}

int test_missing_sequence_keeps_its_reference() {
    MidiSequence missing;
    missing.missing = sequence_ref(example());
    const SequenceFileRef ref = sequence_ref(missing);
    CHECK(ref == *missing.missing);
    return 0;
}

// The memo hashes a sequence once while it lives, and never matches a
// freed one, whose address may come back.
int test_digests() {
    SequenceDigests d;
    auto a = std::make_shared<const MidiSequence>(example());
    std::string encoded;
    const SequenceFileRef first = d.ref(a, &encoded);
    CHECK(first.sha256 == kExampleSha && encoded == from_hex(kExampleHex));
    CHECK(d.ref(a, &encoded) == first && encoded.empty());
    const MidiSequence* where = a.get();
    a.reset();
    // Whatever lands at the old address is hashed afresh.
    for (int i = 0; i < 100; ++i) {
        auto b = std::make_shared<MidiSequence>();
        b->events = {MidiEvent{i, 0xB0, 1, 2}};
        std::shared_ptr<const MidiSequence> cb = b;
        const SequenceFileRef r = d.ref(cb);
        CHECK(r == sequence_ref(*cb));
        if (cb.get() == where) break;
    }
    d.sweep();
    CHECK(d.size() <= 1);
    return 0;
}

int test_named_sequences_once_each() {
    Snapshot s = new_session(settings());
    Ids ids(s);
    AddTrack t;
    t.track = ids();
    t.playlist = ids();
    t.name = "Bass";
    t.playlist_name = "Bass.01";
    t.kind = TrackKind::Midi;
    t.channels = 0;
    History h(s);
    CHECK_APPLY(h, t);
    auto shared = std::make_shared<const MidiSequence>(example());
    MidiRegion r;
    r.id = ids();
    r.name = "a";
    r.length = 4 * kTicksPerQuarter;
    r.data = shared;
    CHECK_APPLY(h, (PlaceMidiRegion{t.playlist, r, 0}));
    MidiDataRef from{t.playlist, r.id};
    MidiRegion copy = r;
    copy.id = ids();
    copy.position = 8 * kTicksPerQuarter;
    copy.data = nullptr;
    PlaceMidiRegion again{t.playlist, copy, 0};
    again.data_from = from;
    CHECK_APPLY(h, again);
    // A second object with the same notes is the same file.
    MidiRegion twin = r;
    twin.id = ids();
    twin.position = 16 * kTicksPerQuarter;
    twin.data = std::make_shared<const MidiSequence>(example());
    CHECK_APPLY(h, (PlaceMidiRegion{t.playlist, twin, 0}));
    SequenceDigests d;
    const std::vector<NamedSequence> named = named_sequences(h.current(), d);
    CHECK(named.size() == 1);
    CHECK(named[0].ref.sha256 == kExampleSha && named[0].encoded == from_hex(kExampleHex));
    return 0;
}

int test_references_from_a_session_file() {
    std::set<std::string> refs;
    std::string why;
    const std::string v6 =
        "{\"sonora_session\": 6, \"tracks\": [{\"playlists\": [{\"regions\": ["
        "{\"data\": {\"sha256\": \"aa\"}}, {\"data\": {\"sha256\": \"bb\"}}]}]}]}";
    CHECK(sequence_references(v6, refs, why));
    CHECK(refs == (std::set<std::string>{"aa", "bb"}));
    // Version 5 kept its MIDI inline: it names no file.
    CHECK(sequence_references("{\"sonora_session\": 5, \"tracks\": []}", refs, why));
    CHECK(refs.empty());
    CHECK(!sequence_references("{\"sonora_session\": 6,", refs, why) && !why.empty());
    CHECK(!sequence_references("{\"other\": 1}", refs, why));
    return 0;
}

// O4: a file no kept session file names goes; while one kept file cannot
// be read, nothing goes.
int test_which_files_may_go() {
    SequenceFileSet set;
    set.kept("main", {"a", "b"});
    set.kept("backup 1", {"b", "c"});
    CHECK((set.deletable({"a", "b", "c", "d"}) == std::vector<std::string>{"d"}));
    set.kept("main", {"b"});   // the main file saved again
    CHECK((set.deletable({"a", "b", "c", "d"}) == std::vector<std::string>{"a", "d"}));
    set.dropped("backup 1");
    CHECK((set.deletable({"a", "b", "c"}) == std::vector<std::string>{"a", "c"}));
    set.kept_unreadable("autosave");
    CHECK(set.deletable({"a", "b", "c"}).empty());
    CHECK(set.unreadable() == std::vector<std::string>{"autosave"});
    set.kept("autosave", {});
    CHECK((set.deletable({"a", "b", "c"}) == std::vector<std::string>{"a", "c"}));
    set.wrote("a");
    CHECK(set.on_disk("a"));
    set.deleted("a");
    CHECK(!set.on_disk("a"));
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    int run = 0;
    const auto t = [&](int (*f)()) {
        ++run;
        failed += f();
    };
    t(test_bytes_are_the_spec);
    t(test_largest_values);
    t(test_refusals);
    t(test_fuzz);
    t(test_missing_sequence_keeps_its_reference);
    t(test_digests);
    t(test_named_sequences_once_each);
    t(test_references_from_a_session_file);
    t(test_which_files_may_go);
    if (failed != 0) {
        std::fprintf(stderr, "model_sequence_file: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("model_sequence_file: %d passed\n", run);
    return 0;
}
