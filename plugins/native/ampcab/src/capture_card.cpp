// SPDX-License-Identifier: GPL-3.0-or-later
//
// Capture cards (capture_card.h, ADR-0069). Off the audio path: allocation
// is fine, exceptions are not (the build check and the loader must list
// every refusal, not stop at the first). Writing is capture_card_format.cpp.
#include "sonora/ampcab/capture_card.h"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>

#include "sonora/json/json.h"
#include "sonora/sha256.h"

namespace sonora::ampcab {
namespace {

using json::Value;
using Refusals = std::vector<std::string>;

// ---- The fenced block ---------------------------------------------------

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

// The one ```card-json block's body. Fences are whole lines, so an inline
// mention of the tag in the prose never opens a block.
bool extract_block(std::string_view md, std::string& body, Refusals& r) {
    int blocks = 0;
    bool inside = false;
    std::string current;
    std::size_t pos = 0;
    while (pos <= md.size()) {
        std::size_t end = md.find('\n', pos);
        if (end == std::string_view::npos) end = md.size();
        const std::string_view raw = md.substr(pos, end - pos);
        const std::string_view line = trim(raw);
        if (!inside && line == "```card-json") {
            inside = true;
            current.clear();
        } else if (inside && line == "```") {
            inside = false;
            if (++blocks == 1) body = current;
        } else if (inside) {
            current.append(raw.data(), raw.size());
            current.push_back('\n');
        }
        pos = end + 1;
    }
    if (inside) r.push_back("the ```card-json block is not closed");
    if (blocks == 0 && !inside) r.push_back("the card has no ```card-json block");
    if (blocks > 1)
        r.push_back("the card has " + std::to_string(blocks) +
                    " ```card-json blocks; it must have exactly one");
    return blocks == 1 && !inside;
}

// ---- Typed field reads ---------------------------------------------------

// Reads one object's members, refusing what is missing, mistyped or unknown
// (an unknown key is most often a misspelt one: `esr_heldout` must not pass
// as "no ESR recorded").
class ObjectReader {
public:
    ObjectReader(const Value& v, std::string path, Refusals& r)
        : v_(v), path_(std::move(path)), r_(r) {}

    const Value* get(std::string_view key, bool required) {
        seen_.emplace_back(key);
        const Value* f = v_.find(key);
        if (!f && required) r_.push_back(where(key) + " is missing");
        return f;
    }
    void str(std::string_view key, std::string& out) {
        if (const Value* f = get(key, true)) {
            if (f->is_string()) out = f->text();
            else r_.push_back(where(key) + " must be a string");
        }
    }
    void boolean(std::string_view key, bool required, bool& out) {
        if (const Value* f = get(key, required)) {
            if (f->is_bool()) out = f->as_bool();
            else r_.push_back(where(key) + " must be true or false");
        }
    }
    bool integer(std::string_view key, std::int64_t lo, std::int64_t hi,
                 std::int64_t& out) {
        const Value* f = get(key, true);
        if (!f) return false;
        if (!f->is_number() || !f->to_int64(out) || out < lo || out > hi) {
            r_.push_back(where(key) + " must be an integer from " +
                         std::to_string(lo) + " to " + std::to_string(hi));
            return false;
        }
        return true;
    }
    const Value* object(std::string_view key) {
        const Value* f = get(key, true);
        if (f && !f->is_object()) {
            r_.push_back(where(key) + " must be an object");
            return nullptr;
        }
        return f;
    }
    // Call last: every key no read named is refused.
    void no_unknown_keys() {
        for (const auto& m : v_.members()) {
            bool known = false;
            for (const auto& k : seen_) known = known || k == m.first;
            if (!known) r_.push_back(where(m.first) + " is not a card field");
        }
    }
    std::string where(std::string_view key) const {
        return (path_.empty() ? "" : path_ + ".") + std::string(key);
    }

private:
    const Value& v_;
    std::string path_;
    Refusals& r_;
    std::vector<std::string> seen_;
};

template <typename E>
bool read_enum(ObjectReader& o, std::string_view key,
               std::initializer_list<std::pair<const char*, E>> names, E& out,
               Refusals& r) {
    std::string s;
    const std::size_t before = r.size();
    o.str(key, s);
    if (r.size() != before) return false;
    std::string allowed;
    for (const auto& n : names) {
        if (s == n.first) {
            out = n.second;
            return true;
        }
        allowed += (allowed.empty() ? "" : "|") + std::string(n.first);
    }
    r.push_back(o.where(key) + " is \"" + s + "\"; expected " + allowed);
    return false;
}

void read_unit(ObjectReader& top, CaptureUnit& u, Refusals& r) {
    const Value* v = top.object("unit");
    if (!v) return;
    ObjectReader o(*v, "unit", r);
    o.str("type", u.type);
    o.str("serial", u.serial);
    read_enum(o, "ownership",
              {{"owned", Ownership::owned}, {"rented", Ownership::rented}},
              u.ownership, r);
    // Absent reads as false; validate_card refuses a rented unit without it.
    o.boolean("rental_terms_allow_recording", false,
              u.rental_terms_allow_recording);
    o.no_unknown_keys();
}

void read_settings(ObjectReader& top,
                   std::vector<std::pair<std::string, std::string>>& out,
                   Refusals& r) {
    const Value* v = top.object("settings");
    if (!v) return;
    for (const auto& m : v->members()) {
        const Value& s = m.second;
        if (s.is_string() || s.is_number()) {
            out.emplace_back(m.first, s.text());
        } else if (s.is_bool()) {
            out.emplace_back(m.first, s.as_bool() ? "true" : "false");
        } else {
            r.push_back("settings." + m.first +
                        " must be a string, a number or a boolean");
        }
    }
}

void read_third_party(ObjectReader& top, ThirdParty& t, Refusals& r) {
    const Value* v = top.get("third_party", false);
    if (!v || v->is_null()) return;
    if (!v->is_object()) {
        r.push_back("third_party must be null or an object");
        return;
    }
    t.present = true;
    ObjectReader o(*v, "third_party", r);
    if (const Value* p = o.get("licence_path", true)) {
        if (p->is_string()) t.licence_path = p->text();
        else if (!p->is_null()) r.push_back("third_party.licence_path must be a string or null");
    }
    o.no_unknown_keys();
}

void read_ir(const Value& v, IrDetails& ir, Refusals& r) {
    ObjectReader o(v, "ir", r);
    o.str("cabinet", ir.cabinet);
    o.str("speaker", ir.speaker);
    o.str("microphone", ir.microphone);
    o.str("position", ir.position);
    o.str("room", ir.room);
    o.no_unknown_keys();
}

void read_capture(const Value& v, CaptureDetails& c, Refusals& r) {
    ObjectReader o(v, "capture", r);
    o.str("architecture", c.architecture);
    std::int64_t rate = 0;
    if (o.integer("sample_rate", 1, 1'536'000, rate))
        c.sample_rate = static_cast<std::uint32_t>(rate);
    // Absent reads as "not recorded"; validate_card refuses that.
    if (const Value* e = o.get("esr_held_out", false)) {
        if (e->is_number() && e->to_double(c.esr_held_out)) c.has_esr_held_out = true;
        else r.push_back("capture.esr_held_out must be a number");
    }
    o.str("train_commit", c.train_commit);
    o.no_unknown_keys();
}

void read_file(ObjectReader& top, CardFile& f, Refusals& r) {
    const Value* v = top.object("file");
    if (!v) return;
    ObjectReader o(*v, "file", r);
    o.str("name", f.name);
    o.str("sha256", f.sha256);
    std::int64_t size = 0;
    if (o.integer("size", 0, INT64_MAX, size)) f.size = static_cast<std::uint64_t>(size);
    o.no_unknown_keys();
}

// The kind's own block is required; the other kind's is refused, so a card
// never carries IR details for a capture by copy-paste.
void read_kind_details(ObjectReader& top, CaptureCard& c, Refusals& r) {
    const bool ir = c.kind == CardKind::ir;
    const char* own = ir ? "ir" : "capture";
    const char* other = ir ? "capture" : "ir";
    if (top.get(other, false))
        r.push_back(std::string(other) + " is not a field of a " + own + " card");
    const Value* v = top.object(own);
    if (!v) return;
    if (ir) read_ir(*v, c.ir, r);
    else read_capture(*v, c.capture, r);
}

// ---- Policy helpers ------------------------------------------------------

bool valid_id(std::string_view id) {
    if (id.empty()) return false;
    for (const char ch : id)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-'))
            return false;
    return true;
}

bool valid_sha256(std::string_view h) {
    if (h.size() != 64) return false;
    for (const char ch : h)
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
    return true;
}

bool valid_date(std::string_view d) {
    if (d.size() != 10 || d[4] != '-' || d[7] != '-') return false;
    for (const std::size_t i : {0, 1, 2, 3, 5, 6, 8, 9})
        if (d[i] < '0' || d[i] > '9') return false;
    const int month = (d[5] - '0') * 10 + (d[6] - '0');
    const int day = (d[8] - '0') * 10 + (d[9] - '0');
    return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

// Lower-case ASCII alphanumerics only: punctuation and spacing are how a
// near-variant hides ("JCM-800", "jcm 800").
std::string normalise(std::string_view s) {
    std::string out;
    for (const char ch : s) {
        if (ch >= 'A' && ch <= 'Z') out.push_back(static_cast<char>(ch - 'A' + 'a'));
        else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) out.push_back(ch);
    }
    return out;
}

void deny_names(const CaptureCard& c, const std::vector<std::string>& deny,
                Refusals& r) {
    const std::pair<const char*, const std::string*> names[] = {
        {"id", &c.id}, {"name", &c.name}, {"file.name", &c.file.name}};
    for (const auto& n : names) {
        const std::string flat = normalise(*n.second);
        for (const auto& term : deny) {
            if (!term.empty() && flat.find(term) != std::string::npos)
                r.push_back(std::string(n.first) + " \"" + *n.second +
                            "\" contains the denied name \"" + term +
                            "\" (ADR-0011 §4.6, §5)");
        }
    }
}

void require_text(const char* field, const std::string& v, Refusals& r) {
    if (trim(v).empty()) r.push_back(std::string(field) + " is empty");
}

}  // namespace

std::vector<std::string> parse_card(std::string_view md, CaptureCard& out) {
    Refusals r;
    out = CaptureCard{};
    std::string body;
    if (!extract_block(md, body, r)) return r;
    Value root;
    json::Error err;
    if (!json::parse(body, root, err)) {
        r.push_back("card-json line " + std::to_string(err.line) + ", column " +
                    std::to_string(err.column) + ": " + err.message);
        return r;
    }
    if (!root.is_object()) {
        r.push_back("the card-json block must hold one object");
        return r;
    }
    ObjectReader top(root, "", r);
    top.str("id", out.id);
    top.str("name", out.name);
    const bool kind_ok = read_enum(
        top, "kind", {{"capture", CardKind::capture}, {"ir", CardKind::ir}},
        out.kind, r);
    read_enum(top, "source",
              {{"hardware", CaptureSource::hardware}, {"plugin", CaptureSource::plugin}},
              out.source, r);
    read_unit(top, out.unit, r);
    read_settings(top, out.settings, r);
    top.str("rig", out.rig);
    top.str("date", out.date);
    read_third_party(top, out.third_party, r);
    if (kind_ok) {
        read_kind_details(top, out, r);
    } else {
        top.get("ir", false);  // named, so not reported as unknown as well
        top.get("capture", false);
    }
    read_file(top, out.file, r);
    top.no_unknown_keys();
    return r;
}

std::vector<std::string> parse_name_deny(std::string_view text) {
    std::vector<std::string> terms;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(pos, end - pos);
        const std::size_t hash = line.find('#');
        if (hash != std::string_view::npos) line = line.substr(0, hash);
        std::string term = normalise(line);
        if (!term.empty()) terms.push_back(std::move(term));
        pos = end + 1;
    }
    return terms;
}

std::vector<std::string> validate_card(const CaptureCard& c,
                                       const std::vector<std::string>& deny) {
    Refusals r;
    if (!valid_id(c.id)) r.push_back("id \"" + c.id + "\" must match ^[a-z0-9-]+$");
    require_text("name", c.name, r);
    if (c.source != CaptureSource::hardware)
        r.push_back("source is a plugin: only hardware we own or rent is captured "
                    "(ADR-0011 §4.6)");
    require_text("unit.type", c.unit.type, r);
    require_text("unit.serial", c.unit.serial, r);
    if (c.unit.ownership == Ownership::rented && !c.unit.rental_terms_allow_recording)
        r.push_back("unit is rented but unit.rental_terms_allow_recording is not "
                    "true");
    require_text("rig", c.rig, r);
    if (!valid_date(c.date)) r.push_back("date \"" + c.date + "\" must be YYYY-MM-DD");
    if (c.third_party.present && trim(c.third_party.licence_path).empty())
        r.push_back("third_party has no licence_path: a third-party capture needs "
                    "a written licence on the card");
    if (c.kind == CardKind::ir) {
        require_text("ir.cabinet", c.ir.cabinet, r);
        require_text("ir.speaker", c.ir.speaker, r);
        require_text("ir.microphone", c.ir.microphone, r);
        require_text("ir.position", c.ir.position, r);
        require_text("ir.room", c.ir.room, r);
    } else {
        require_text("capture.architecture", c.capture.architecture, r);
        if (c.capture.sample_rate == 0) r.push_back("capture.sample_rate is 0");
        if (!c.capture.has_esr_held_out)
            r.push_back("capture.esr_held_out is missing: a capture states its "
                        "held-out error");
        else if (!std::isfinite(c.capture.esr_held_out) || c.capture.esr_held_out < 0)
            r.push_back("capture.esr_held_out must be a finite number >= 0");
        require_text("capture.train_commit", c.capture.train_commit, r);
    }
    require_text("file.name", c.file.name, r);
    if (!valid_sha256(c.file.sha256))
        r.push_back("file.sha256 must be 64 lower-case hex digits");
    if (c.file.size > kMaxCaptureFileBytes)
        r.push_back("file.size " + std::to_string(c.file.size) + " B is over the " +
                    std::to_string(kMaxCaptureFileBytes) + " B bound per file");
    deny_names(c, deny, r);
    return r;
}

std::vector<std::string> check_capture_file(const CaptureCard& card,
                                            std::string_view bytes) {
    Refusals r;
    if (bytes.size() != card.file.size)
        r.push_back(card.file.name + " is " + std::to_string(bytes.size()) +
                    " B; its card says " + std::to_string(card.file.size) + " B");
    const std::string got = sha256_hex(bytes);
    if (got != card.file.sha256)
        r.push_back(card.file.name + " has SHA-256 " + got + "; its card says " +
                    card.file.sha256);
    return r;
}

}  // namespace sonora::ampcab
