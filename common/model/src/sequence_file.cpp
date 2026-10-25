// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/model/sequence_file.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

#include "checks.h"
#include "sonora/json/json.h"
#include "sonora/sha256.h"

namespace sonora::model {

namespace {

constexpr char kMagic[4] = {'S', 'M', 'S', 'Q'};
constexpr std::size_t kHeaderBytes = sizeof kMagic + 1;
// The fewest bytes an entry encodes to: a one-byte delta and length, then
// its fixed bytes. What bounds a count before anything is reserved.
constexpr std::uint64_t kMinNoteBytes = 1 + 1 + 4;
constexpr std::uint64_t kMinEventBytes = 1 + 3;
constexpr std::uint64_t kMinSysexBytes = 1 + 1 + 2;
// 2^53 - 1 needs 53 bits: eight 7-bit groups.
constexpr int kMaxLebBytes = 8;

void put_leb(std::string& out, std::uint64_t v) {
    do {
        const auto low = static_cast<unsigned char>(v & 0x7F);
        v >>= 7;
        out.push_back(static_cast<char>(v != 0 ? low | 0x80 : low));
    } while (v != 0);
}

void put_byte(std::string& out, std::uint8_t b) { out.push_back(static_cast<char>(b)); }

class Decoder {
public:
    Decoder(std::string_view bytes, std::string& error) : p_(bytes), error_(error) {}

    bool fail(const std::string& why) {
        if (ok_) error_ = why + " (at byte " + std::to_string(at_) + ")";
        ok_ = false;
        return false;
    }
    bool ok() const { return ok_; }
    std::size_t left() const { return p_.size() - at_; }
    bool done() {
        if (ok_ && left() != 0) return fail("bytes after the last entry");
        return ok_;
    }

    bool byte(std::uint8_t& out) {
        if (!ok_) return false;
        if (left() == 0) return fail("ends inside an entry");
        out = static_cast<std::uint8_t>(p_[at_++]);
        return true;
    }

    // Minimal unsigned LEB128, at most kMaxInteger: one value, one spelling.
    bool leb(std::uint64_t& out, const char* what) {
        if (!ok_) return false;
        std::uint64_t v = 0;
        for (int i = 0; i < kMaxLebBytes; ++i) {
            std::uint8_t b = 0;
            if (!byte(b)) return false;
            v |= static_cast<std::uint64_t>(b & 0x7F) << (7 * i);
            if ((b & 0x80) == 0) {
                if (i > 0 && b == 0) return fail(std::string(what) + " is not minimal LEB128");
                if (v > static_cast<std::uint64_t>(kMaxInteger)) {
                    return fail(std::string(what) + " is past 2^53 - 1");
                }
                out = v;
                return true;
            }
        }
        return fail(std::string(what) + " is past 2^53 - 1");
    }

    // The next entry's tick: the previous one's plus a delta, never past
    // the session file's integer range.
    bool tick(std::int64_t& prev, const char* what) {
        std::uint64_t d = 0;
        if (!leb(d, what)) return false;
        if (d > static_cast<std::uint64_t>(kMaxInteger - prev)) {
            return fail(std::string(what) + " is past 2^53 - 1");
        }
        prev += static_cast<std::int64_t>(d);
        return true;
    }

    bool raw(std::size_t n, std::vector<std::uint8_t>& out) {
        if (!ok_) return false;
        if (left() < n) return fail("ends inside an entry");
        const auto* b = reinterpret_cast<const std::uint8_t*>(p_.data() + at_);
        out.assign(b, b + n);
        at_ += n;
        return true;
    }

    bool magic() {
        if (p_.size() < kHeaderBytes || !std::equal(kMagic, kMagic + 4, p_.data())) {
            return fail("not a Sonora MIDI sequence (magic)");
        }
        at_ = sizeof kMagic;
        std::uint8_t version = 0;
        byte(version);
        if (version != kSequenceFileVersion) {
            return fail("sequence file version " + std::to_string(version) +
                        "; this build reads version " + std::to_string(kSequenceFileVersion));
        }
        return true;
    }

private:
    std::string_view p_;
    std::size_t at_ = 0;
    bool ok_ = true;
    std::string& error_;
};

}  // namespace

namespace {

template <typename Notes, typename Events, typename Sysex>
void encode_into(std::string& out, const Notes& notes, const Events& events,
                 const Sysex& sysex) {
    out.reserve(out.size() + kHeaderBytes + 3 + notes.size() * 10 + events.size() * 6);
    out.append(kMagic, sizeof kMagic);
    put_byte(out, kSequenceFileVersion);
    put_leb(out, notes.size());
    put_leb(out, events.size());
    put_leb(out, sysex.size());
    // Each section is in non-decreasing order of its first key, so every
    // delta is unsigned.
    std::int64_t prev = 0;
    for (const midi::Note& n : notes) {
        put_leb(out, static_cast<std::uint64_t>(n.start - prev));
        prev = n.start;
        put_leb(out, static_cast<std::uint64_t>(n.length));
        put_byte(out, n.channel);
        put_byte(out, n.pitch);
        put_byte(out, n.velocity);
        put_byte(out, n.release_velocity);
    }
    prev = 0;
    for (const MidiEvent& e : events) {
        put_leb(out, static_cast<std::uint64_t>(e.tick - prev));
        prev = e.tick;
        put_byte(out, e.status);
        put_byte(out, e.data1);
        put_byte(out, e.data2);
    }
    prev = 0;
    for (const MidiSysEx& x : sysex) {
        put_leb(out, static_cast<std::uint64_t>(x.tick - prev));
        prev = x.tick;
        put_leb(out, x.bytes.size());
        out.append(reinterpret_cast<const char*>(x.bytes.data()), x.bytes.size());
    }
}

}  // namespace

std::string encode_sequence(const MidiSequence& s) {
    std::string out;
    encode_into(out, s.notes(), s.events(), s.sysex());
    return out;
}

std::string encode_lists(const MidiLists& s) {
    std::string out;
    encode_into(out, s.notes, s.events, s.sysex);
    return out;
}

bool decode_lists(std::string_view bytes, MidiLists& out, std::string& error) {
    Decoder d(bytes, error);
    if (!d.magic()) return false;
    std::uint64_t notes = 0, events = 0, sysex = 0;
    if (!d.leb(notes, "the note count") || !d.leb(events, "the event count") ||
        !d.leb(sysex, "the SysEx count")) {
        return false;
    }
    // Checked before anything is reserved, so a hostile count costs nothing.
    const std::uint64_t left = d.left();
    if (notes > left / kMinNoteBytes || events > left / kMinEventBytes ||
        sysex > left / kMinSysexBytes ||
        notes * kMinNoteBytes + events * kMinEventBytes + sysex * kMinSysexBytes > left) {
        return d.fail("the counts promise more entries than the file holds");
    }
    MidiLists s;
    s.notes.reserve(static_cast<std::size_t>(notes));
    s.events.reserve(static_cast<std::size_t>(events));
    s.sysex.reserve(static_cast<std::size_t>(sysex));
    std::int64_t prev = 0;
    for (std::uint64_t i = 0; i < notes && d.ok(); ++i) {
        midi::Note n;
        std::uint64_t length = 0;
        if (!d.tick(prev, "a note's start") || !d.leb(length, "a note's length")) break;
        n.start = prev;
        n.length = static_cast<std::int64_t>(length);
        d.byte(n.channel);
        d.byte(n.pitch);
        d.byte(n.velocity);
        d.byte(n.release_velocity);
        s.notes.push_back(n);
    }
    prev = 0;
    for (std::uint64_t i = 0; i < events && d.ok(); ++i) {
        MidiEvent e;
        if (!d.tick(prev, "an event's tick")) break;
        e.tick = prev;
        d.byte(e.status);
        d.byte(e.data1);
        d.byte(e.data2);
        s.events.push_back(e);
    }
    prev = 0;
    for (std::uint64_t i = 0; i < sysex && d.ok(); ++i) {
        MidiSysEx x;
        std::uint64_t size = 0;
        if (!d.tick(prev, "a SysEx message's tick") || !d.leb(size, "a SysEx message's size")) {
            break;
        }
        if (size > kMaxSysexBytes) {
            d.fail("a SysEx message is at most " + std::to_string(kMaxSysexBytes) + " bytes");
            break;
        }
        x.tick = prev;
        if (!d.raw(static_cast<std::size_t>(size), x.bytes)) break;
        s.sysex.push_back(std::move(x));
    }
    if (!d.done()) return false;
    std::string where;
    const std::string why = detail::sequence_problem(s, where);
    if (!why.empty()) {
        error = where + ": " + why;
        return false;
    }
    out = std::move(s);
    return true;
}

bool decode_sequence(std::string_view bytes, MidiSequence& out, std::string& error) {
    MidiLists lists;
    if (!decode_lists(bytes, lists, error)) return false;
    out = MidiSequence(std::move(lists));
    return true;
}

// --- the undo window's chunk table, in parts --------------------------------

namespace {

constexpr char kPartMagic[4] = {'S', 'W', 'C', 'P'};
constexpr std::uint8_t kPartVersion = 1;

Ticks tick_of(const midi::Note& n) { return n.start; }
Ticks tick_of(const MidiEvent& e) { return e.tick; }
Ticks tick_of(const MidiSysEx& x) { return x.tick; }

// A list in order is in the span when its ends are.
template <typename T>
bool in_span(const std::vector<T>& v, std::int64_t chunk) {
    return v.empty() || (midi_chunk_of(tick_of(v.front())) == chunk &&
                         midi_chunk_of(tick_of(v.back())) == chunk);
}

std::size_t leb_size(std::uint64_t v) {
    std::size_t n = 1;
    while (v >= 0x80) {
        v >>= 7;
        ++n;
    }
    return n;
}

}  // namespace

std::string oversized_chunk(const MidiChunk& c, const std::string& track) {
    // A bound on the encoding first (a note is at most 20 bytes, an event
    // 11, a SysEx message 10 and its own), so a chunk is encoded only when
    // it may be too large.
    std::size_t bound = 64 + 32 * static_cast<std::size_t>(c.data.entries());
    for (const MidiSysEx& x : c.data.sysex) bound += x.bytes.size();
    if (bound <= kMaxChunkBytes) return {};
    const std::size_t bytes = encode_lists(c.data).size();
    if (bytes <= kMaxChunkBytes) return {};
    return "track \"" + track + "\": the MIDI in chunk " + std::to_string(c.index) +
           " (four beats from beat " + std::to_string(c.index * 4) + ") would be " +
           std::to_string(bytes) + " bytes, over the " + std::to_string(kMaxChunkBytes) +
           " one chunk may hold; spread it over more time";
}

std::string oversized_chunk(const EditState& state) {
    std::unordered_set<const void*> seen;
    for (const auto& t : state.tracks) {
        for (const auto& p : t->midi_playlists) {
            if (!seen.insert(p.get()).second) continue;
            for (const MidiRegion& r : p->regions) {
                if (!r.data || !seen.insert(r.data.get()).second) continue;
                for (const MidiSpineNode& n : r.data->spine()) {
                    if (!seen.insert(n.leaf.get()).second) continue;
                    for (const MidiSpineEntry& e : n.leaf->chunks) {
                        if (!seen.insert(e.chunk.get()).second) continue;
                        std::string why = oversized_chunk(*e.chunk, t->name);
                        if (!why.empty()) return why;
                    }
                }
            }
        }
    }
    return {};
}

bool encode_chunk_parts(const std::vector<const MidiChunk*>& table, std::size_t max_part_bytes,
                        std::vector<std::string>& parts, std::string& error) {
    parts.clear();
    std::string part, count_and_chunks;
    std::size_t first = 0, in_part = 0;
    const auto header_bytes = [&](std::size_t at, std::size_t n) {
        return sizeof kPartMagic + 1 + leb_size(at) + leb_size(n);
    };
    const auto close = [&]() {
        part.assign(kPartMagic, sizeof kPartMagic);
        put_byte(part, kPartVersion);
        put_leb(part, first);
        put_leb(part, in_part);
        part += count_and_chunks;
        parts.push_back(std::move(part));
        part.clear();
        count_and_chunks.clear();
        first += in_part;
        in_part = 0;
    };
    std::string one;
    for (std::size_t i = 0; i < table.size(); ++i) {
        const MidiChunk& c = *table[i];
        one.clear();
        put_leb(one, static_cast<std::uint64_t>(c.index));
        const std::string msq = encode_lists(c.data);
        put_leb(one, msq.size());
        one += msq;
        if (header_bytes(i, 1) + one.size() > max_part_bytes) {
            error = "chunk " + std::to_string(c.index) + " encodes to " +
                    std::to_string(one.size()) + " bytes, over the " +
                    std::to_string(max_part_bytes) + " a part of the chunk table may hold";
            return false;
        }
        if (in_part > 0 &&
            header_bytes(first, in_part + 1) + count_and_chunks.size() + one.size() >
                max_part_bytes) {
            close();
        }
        count_and_chunks += one;
        ++in_part;
    }
    if (in_part > 0) close();
    return true;
}

bool decode_chunk_part(std::string_view bytes, std::vector<std::shared_ptr<const MidiChunk>>& table,
                       std::string& error) {
    if (bytes.size() < sizeof kPartMagic + 1 ||
        !std::equal(kPartMagic, kPartMagic + 4, bytes.data())) {
        error = "not a part of the undo window's chunk table (magic)";
        return false;
    }
    if (static_cast<std::uint8_t>(bytes[4]) != kPartVersion) {
        error = "chunk table part version " +
                std::to_string(static_cast<std::uint8_t>(bytes[4])) + "; this build reads " +
                std::to_string(kPartVersion);
        return false;
    }
    Decoder d(bytes.substr(sizeof kPartMagic + 1), error);
    std::uint64_t first = 0, count = 0;
    if (!d.leb(first, "the part's first index") || !d.leb(count, "the part's chunk count")) {
        return false;
    }
    if (first != table.size()) {
        return d.fail("the part starts at chunk " + std::to_string(first) + " and " +
                      std::to_string(table.size()) + " have arrived");
    }
    // A chunk is at least its index, its size and an empty .msq.
    if (count == 0 || count > d.left() / (2 + kHeaderBytes + 3)) {
        return d.fail("the part's chunk count does not fit its bytes");
    }
    std::vector<std::shared_ptr<const MidiChunk>> got;
    got.reserve(static_cast<std::size_t>(count));
    std::vector<std::uint8_t> msq;
    for (std::uint64_t i = 0; i < count; ++i) {
        std::uint64_t index = 0, size = 0;
        if (!d.leb(index, "a chunk's index") || !d.leb(size, "a chunk's size")) return false;
        if (index > static_cast<std::uint64_t>(kMaxInteger / kMidiChunkTicks)) {
            return d.fail("chunk index " + std::to_string(index) + " is past the timeline");
        }
        if (size > d.left()) return d.fail("a chunk is cut short");
        if (!d.raw(static_cast<std::size_t>(size), msq)) return false;
        auto c = std::make_shared<MidiChunk>();
        c->index = static_cast<std::int64_t>(index);
        std::string why;
        if (!decode_lists(std::string_view(reinterpret_cast<const char*>(msq.data()), msq.size()),
                          c->data, why)) {
            return d.fail("chunk " + std::to_string(first + i) + ": " + why);
        }
        if (c->data.empty()) return d.fail("chunk " + std::to_string(first + i) + " is empty");
        const MidiLists& l = c->data;
        if (!in_span(l.notes, c->index) || !in_span(l.events, c->index) ||
            !in_span(l.sysex, c->index)) {
            return d.fail("chunk " + std::to_string(first + i) + " holds data outside chunk " +
                          std::to_string(index) + "'s span");
        }
        got.push_back(std::move(c));
    }
    if (!d.done()) return false;
    table.insert(table.end(), std::make_move_iterator(got.begin()),
                 std::make_move_iterator(got.end()));
    return true;
}

SequenceFileRef sequence_ref(const MidiSequence& s, std::string* encoded) {
    if (s.missing) return *s.missing;
    std::string bytes = encode_sequence(s);
    SequenceFileRef r;
    r.sha256 = sha256_hex(bytes);
    r.file = midi_sequence_file(r.sha256);
    r.bytes = bytes.size();
    r.notes = s.note_count();
    r.events = s.event_count();
    r.sysex = s.sysex_count();
    if (encoded != nullptr) *encoded = std::move(bytes);
    return r;
}

const SequenceFileRef& SequenceDigests::ref(const std::shared_ptr<const MidiSequence>& s,
                                            std::string* encoded) {
    if (encoded != nullptr) encoded->clear();
    auto it = memo_.find(s.get());
    if (it != memo_.end()) {
        // Still the same object: one that is alive at this address cannot
        // be another.
        if (!it->second.seq.expired()) return it->second.ref;
        memo_.erase(it);
    }
    if (memo_.size() >= 2 * swept_at_) sweep();
    Entry e{s, sequence_ref(*s, encoded)};
    return memo_.emplace(s.get(), std::move(e)).first->second.ref;
}

void SequenceDigests::sweep() {
    for (auto it = memo_.begin(); it != memo_.end();) {
        it = it->second.seq.expired() ? memo_.erase(it) : std::next(it);
    }
    swept_at_ = std::max<std::size_t>(64, memo_.size());
}

std::vector<NamedSequence> named_sequences(const Snapshot& s, SequenceDigests& digests) {
    std::vector<NamedSequence> out;
    std::unordered_set<const MidiSequence*> seen;
    std::unordered_set<std::string> names;
    for (const auto& t : s.edit->tracks) {
        for (const auto& p : t->midi_playlists) {
            for (const MidiRegion& r : p->regions) {
                if (!seen.insert(r.data.get()).second) continue;
                NamedSequence n;
                n.data = r.data;
                n.ref = digests.ref(r.data, &n.encoded);
                // Two objects with the same notes are one file.
                if (!names.insert(n.ref.sha256).second) continue;
                out.push_back(std::move(n));
            }
        }
    }
    return out;
}

namespace {

// The root of a session file and its format version, without loading it.
bool session_root(std::string_view session_file, json::Value& root, std::int64_t& version,
                  std::string& why) {
    json::Error perr;
    if (!json::parse(session_file, root, perr)) {
        why = "line " + std::to_string(perr.line) + ", column " + std::to_string(perr.column) +
              ": " + perr.message;
        return false;
    }
    if (!root.is_object() || root.members().empty() ||
        root.members().front().first != "sonora_session") {
        why = "not a Sonora session";
        return false;
    }
    const json::Value& v = root.members().front().second;
    if (!v.is_integer_lexeme() || !v.to_int64(version)) {
        why = "its format version is not an integer";
        return false;
    }
    return true;
}

}  // namespace

bool sequence_references(std::string_view session_file, std::set<std::string>& out,
                         std::string& why) {
    out.clear();
    json::Value root;
    std::int64_t version = 0;
    if (!session_root(session_file, root, version, why)) return false;
    if (version < 6) return true;
    const json::Value* tracks = root.find("tracks");
    if (tracks == nullptr || !tracks->is_array()) return true;
    for (const json::Value& t : tracks->items()) {
        const json::Value* pls = t.is_object() ? t.find("playlists") : nullptr;
        if (pls == nullptr || !pls->is_array()) continue;
        for (const json::Value& p : pls->items()) {
            const json::Value* rs = p.is_object() ? p.find("regions") : nullptr;
            if (rs == nullptr || !rs->is_array()) continue;
            for (const json::Value& r : rs->items()) {
                const json::Value* data = r.is_object() ? r.find("data") : nullptr;
                const json::Value* sha =
                    data != nullptr && data->is_object() ? data->find("sha256") : nullptr;
                if (sha != nullptr && sha->is_string()) out.insert(sha->text());
            }
        }
    }
    return true;
}

bool plugin_state_references(std::string_view session_file, std::set<std::string>& out,
                             std::string& why) {
    out.clear();
    json::Value root;
    std::int64_t version = 0;
    if (!session_root(session_file, root, version, why)) return false;
    const json::Value* tracks = root.find("tracks");
    if (tracks == nullptr || !tracks->is_array()) return true;
    for (const json::Value& t : tracks->items()) {
        const json::Value* ins = t.is_object() ? t.find("inserts") : nullptr;
        if (ins == nullptr || !ins->is_array()) continue;
        for (const json::Value& in : ins->items()) {
            const json::Value* st = in.is_object() ? in.find("state") : nullptr;
            const json::Value* sha = st != nullptr && st->is_object() ? st->find("sha256") : nullptr;
            if (sha != nullptr && sha->is_string()) out.insert(sha->text());
        }
    }
    return true;
}

void SequenceFileSet::kept(const std::string& name, std::set<std::string> references) {
    unreadable_.erase(name);
    refs_[name] = std::move(references);
}

void SequenceFileSet::kept_unreadable(const std::string& name) {
    refs_.erase(name);
    unreadable_.insert(name);
}

void SequenceFileSet::dropped(const std::string& name) {
    refs_.erase(name);
    unreadable_.erase(name);
}

std::vector<std::string> SequenceFileSet::unreadable() const {
    return {unreadable_.begin(), unreadable_.end()};
}

std::vector<std::string> SequenceFileSet::deletable(const std::vector<std::string>& present) const {
    std::vector<std::string> out;
    if (!unreadable_.empty()) return out;
    for (const std::string& sha : present) {
        bool named = false;
        for (const auto& kv : refs_) named = named || kv.second.count(sha) != 0;
        if (!named) out.push_back(sha);
    }
    return out;
}

void SequenceFileSet::clear() {
    written_.clear();
    refs_.clear();
    unreadable_.clear();
}

}  // namespace sonora::model
