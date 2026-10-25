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

std::string encode_sequence(const MidiSequence& s) {
    std::string out;
    out.reserve(kHeaderBytes + 3 + s.notes.size() * 10 + s.events.size() * 6);
    out.append(kMagic, sizeof kMagic);
    put_byte(out, kSequenceFileVersion);
    put_leb(out, s.notes.size());
    put_leb(out, s.events.size());
    put_leb(out, s.sysex.size());
    // Each section is in non-decreasing order of its first key, so every
    // delta is unsigned.
    std::int64_t prev = 0;
    for (const midi::Note& n : s.notes) {
        put_leb(out, static_cast<std::uint64_t>(n.start - prev));
        prev = n.start;
        put_leb(out, static_cast<std::uint64_t>(n.length));
        put_byte(out, n.channel);
        put_byte(out, n.pitch);
        put_byte(out, n.velocity);
        put_byte(out, n.release_velocity);
    }
    prev = 0;
    for (const MidiEvent& e : s.events) {
        put_leb(out, static_cast<std::uint64_t>(e.tick - prev));
        prev = e.tick;
        put_byte(out, e.status);
        put_byte(out, e.data1);
        put_byte(out, e.data2);
    }
    prev = 0;
    for (const MidiSysEx& x : s.sysex) {
        put_leb(out, static_cast<std::uint64_t>(x.tick - prev));
        prev = x.tick;
        put_leb(out, x.bytes.size());
        out.append(reinterpret_cast<const char*>(x.bytes.data()), x.bytes.size());
    }
    return out;
}

bool decode_sequence(std::string_view bytes, MidiSequence& out, std::string& error) {
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
    MidiSequence s;
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

SequenceFileRef sequence_ref(const MidiSequence& s, std::string* encoded) {
    if (s.missing) return *s.missing;
    std::string bytes = encode_sequence(s);
    SequenceFileRef r;
    r.sha256 = sha256_hex(bytes);
    r.file = midi_sequence_file(r.sha256);
    r.bytes = bytes.size();
    r.notes = s.notes.size();
    r.events = s.events.size();
    r.sysex = s.sysex.size();
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

bool sequence_references(std::string_view session_file, std::set<std::string>& out,
                         std::string& why) {
    out.clear();
    json::Value root;
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
    std::int64_t version = 0;
    const json::Value& v = root.members().front().second;
    if (!v.is_integer_lexeme() || !v.to_int64(version)) {
        why = "its format version is not an integer";
        return false;
    }
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
