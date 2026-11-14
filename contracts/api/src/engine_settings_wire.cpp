// SPDX-License-Identifier: GPL-3.0-or-later
//
// The engine settings pair on the wire (protocol 71, ADR-0204). Its own file
// so wire.cpp does not grow: the layout is wire.cpp's (a type byte, then the
// fields little-endian, a string as a u32 length and its bytes), and the
// messages are fixed apart from two strings, so a small reader of its own is
// shorter than sharing wire.cpp's.
#include <string>

#include "sonora/api/engine_settings.h"
#include "sonora/api/wire.h"

namespace sonora::api::wire {

namespace {

void put(std::string& out, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

void put_str(std::string& out, const std::string& s) {
    put(out, s.size(), 4);
    out.append(s);
}

void put_settings(std::string& out, const EngineSettings& s) {
    put(out, s.autosave_interval_s, 4);
    put(out, s.backup_count, 4);
    put(out, s.automatch_ms, 4);
    put_str(out, s.device_id);
    put(out, s.buffer_frames, 4);
}

class Reader {
public:
    Reader(std::string_view in, std::string& error) : in_(in), error_(error) {}

    bool type(Type expected) {
        if (in_.empty()) return fail("empty message");
        if (static_cast<uint8_t>(in_[0]) != static_cast<uint8_t>(expected)) {
            return fail("message type " + std::to_string(static_cast<uint8_t>(in_[0])) +
                        " where type " + std::to_string(static_cast<uint8_t>(expected)) +
                        " was expected");
        }
        pos_ = 1;
        return true;
    }
    bool u32(uint32_t& v) {
        uint64_t w = 0;
        if (!take(w, 4)) return false;
        v = static_cast<uint32_t>(w);
        return true;
    }
    bool u64(uint64_t& v) { return take(v, 8); }
    bool str(std::string& s, std::size_t cap, const char* what) {
        uint32_t n = 0;
        if (!u32(n)) return false;
        if (n > cap) {
            return fail(std::string(what) + " of " + std::to_string(n) +
                        " bytes, over the cap of " + std::to_string(cap));
        }
        if (in_.size() - pos_ < n) return fail("message cut short");
        s.assign(in_.data() + pos_, n);
        pos_ += n;
        return true;
    }
    bool settings(EngineSettings& s) {
        return u32(s.autosave_interval_s) && u32(s.backup_count) && u32(s.automatch_ms) &&
               str(s.device_id, kMaxDeviceIdBytes, "an audio device id") && u32(s.buffer_frames);
    }
    bool done() {
        if (pos_ != in_.size()) return fail(std::to_string(in_.size() - pos_) + " bytes left over");
        return true;
    }
    bool fail(const std::string& why) {
        error_ = why;
        return false;
    }

private:
    bool take(uint64_t& v, int bytes) {
        if (in_.size() - pos_ < static_cast<std::size_t>(bytes)) return fail("message cut short");
        v = 0;
        for (int i = 0; i < bytes; ++i) {
            v |= static_cast<uint64_t>(static_cast<uint8_t>(in_[pos_ + i])) << (8 * i);
        }
        pos_ += static_cast<std::size_t>(bytes);
        return true;
    }

    std::string_view in_;
    std::string& error_;
    std::size_t pos_ = 0;
};

}  // namespace

void encode(const EngineSettingsSet& m, std::string& out) {
    out.clear();
    out.push_back(static_cast<char>(Type::EngineSettingsSet));
    put(out, m.seq, 8);
    put_settings(out, m.settings);
}

void encode(const EngineSettingsReport& m, std::string& out) {
    out.clear();
    out.push_back(static_cast<char>(Type::EngineSettingsReport));
    put(out, m.seq, 8);
    put_settings(out, m.settings);
    put(out, static_cast<uint32_t>(m.refusal), 4);
    put_str(out, m.reason);
    put(out, m.pending, 4);
}

bool decode(std::string_view bytes, EngineSettingsSet& out, std::string& error) {
    Reader r(bytes, error);
    EngineSettingsSet m;
    if (!r.type(Type::EngineSettingsSet) || !r.u64(m.seq) || !r.settings(m.settings) ||
        !r.done()) {
        return false;
    }
    out = std::move(m);
    return true;
}

bool decode(std::string_view bytes, EngineSettingsReport& out, std::string& error) {
    Reader r(bytes, error);
    EngineSettingsReport m;
    uint32_t refusal = 0;
    if (!r.type(Type::EngineSettingsReport) || !r.u64(m.seq) || !r.settings(m.settings) ||
        !r.u32(refusal) || !r.str(m.reason, kMaxText, "a refusal reason") || !r.u32(m.pending) ||
        !r.done()) {
        return false;
    }
    if (refusal > kLastSettingsRefusal) {
        return r.fail("settings refusal " + std::to_string(refusal) + " is unknown here");
    }
    m.refusal = static_cast<SettingsRefusal>(refusal);
    if ((m.pending & ~kSettingsPendingFlags) != 0) {
        return r.fail("settings pending flags " + std::to_string(m.pending) +
                      " name a flag this version does not define");
    }
    if ((m.refusal == SettingsRefusal::None) != m.reason.empty()) {
        return r.fail(m.reason.empty() ? "a settings refusal with no reason"
                                       : "a settings reason with no refusal");
    }
    out = std::move(m);
    return true;
}

}  // namespace sonora::api::wire
