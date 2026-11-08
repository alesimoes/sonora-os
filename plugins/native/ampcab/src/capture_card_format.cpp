// SPDX-License-Identifier: GPL-3.0-or-later
//
// Capture cards, the writing side (capture_card.h, ADR-0069): format_card,
// whose output parse_card reads back unchanged, and card equality, which is
// what "unchanged" means. Off the audio path.
#include <charconv>
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>

#include "sonora/ampcab/capture_card.h"
#include "sonora/json/json.h"

namespace sonora::ampcab {
namespace {

using json::Value;

Value number_of(double d) {
    char buf[32];
    const auto res = std::to_chars(buf, buf + sizeof buf, d);
    return Value::number_lexeme(std::string(buf, res.ptr));
}

Value settings_value(const std::string& s) {
    // A setting whose text is a JSON number lexeme is written as a number,
    // anything else as a string; either way parse_card restores the same
    // text. The writer is the grammar check ("007", "inf" stay strings).
    Value n = Value::number_lexeme(s);
    std::string text, error;
    if (!s.empty() && json::write(n, text, error)) return n;
    return Value::string(s);
}

}  // namespace

bool operator==(const CaptureCard& a, const CaptureCard& b) {
    const auto unit = [](const CaptureUnit& u) {
        return std::tie(u.type, u.serial, u.ownership, u.rental_terms_allow_recording);
    };
    const auto ir = [](const IrDetails& i) {
        return std::tie(i.cabinet, i.speaker, i.microphone, i.position, i.room);
    };
    const auto cap = [](const CaptureDetails& c) {
        return std::tie(c.architecture, c.sample_rate, c.has_esr_held_out,
                        c.esr_held_out, c.train_commit);
    };
    return a.id == b.id && a.name == b.name && a.kind == b.kind &&
           a.source == b.source && unit(a.unit) == unit(b.unit) &&
           a.settings == b.settings && a.rig == b.rig && a.date == b.date &&
           a.third_party.present == b.third_party.present &&
           a.third_party.licence_path == b.third_party.licence_path &&
           ir(a.ir) == ir(b.ir) && cap(a.capture) == cap(b.capture) &&
           a.file.name == b.file.name && a.file.sha256 == b.file.sha256 &&
           a.file.size == b.file.size;
}

std::string format_card(const CaptureCard& c) {
    Value root = Value::object();
    root.set("id", Value::string(c.id));
    root.set("name", Value::string(c.name));
    root.set("kind", Value::string(c.kind == CardKind::ir ? "ir" : "capture"));
    root.set("source", Value::string(c.source == CaptureSource::hardware ? "hardware"
                                                                         : "plugin"));
    Value unit = Value::object();
    unit.set("type", Value::string(c.unit.type));
    unit.set("serial", Value::string(c.unit.serial));
    const bool rented = c.unit.ownership == Ownership::rented;
    unit.set("ownership", Value::string(rented ? "rented" : "owned"));
    if (rented || c.unit.rental_terms_allow_recording)
        unit.set("rental_terms_allow_recording",
                 Value::boolean(c.unit.rental_terms_allow_recording));
    root.set("unit", std::move(unit));
    Value settings = Value::object();
    for (const auto& s : c.settings) settings.set(s.first, settings_value(s.second));
    root.set("settings", std::move(settings));
    root.set("rig", Value::string(c.rig));
    root.set("date", Value::string(c.date));
    if (c.third_party.present) {
        Value t = Value::object();
        t.set("licence_path", c.third_party.licence_path.empty()
                                  ? Value::null()
                                  : Value::string(c.third_party.licence_path));
        root.set("third_party", std::move(t));
    } else {
        root.set("third_party", Value::null());
    }
    if (c.kind == CardKind::ir) {
        Value ir = Value::object();
        ir.set("cabinet", Value::string(c.ir.cabinet));
        ir.set("speaker", Value::string(c.ir.speaker));
        ir.set("microphone", Value::string(c.ir.microphone));
        ir.set("position", Value::string(c.ir.position));
        ir.set("room", Value::string(c.ir.room));
        root.set("ir", std::move(ir));
    } else {
        Value cap = Value::object();
        cap.set("architecture", Value::string(c.capture.architecture));
        cap.set("sample_rate", Value::integer(c.capture.sample_rate));
        if (c.capture.has_esr_held_out)
            cap.set("esr_held_out", number_of(c.capture.esr_held_out));
        cap.set("train_commit", Value::string(c.capture.train_commit));
        root.set("capture", std::move(cap));
    }
    Value file = Value::object();
    file.set("name", Value::string(c.file.name));
    file.set("sha256", Value::string(c.file.sha256));
    file.set("size", Value::integer(static_cast<std::int64_t>(c.file.size)));
    root.set("file", std::move(file));

    std::string text, error;
    if (!json::write(root, text, error)) text = "{}\n";  // only non-finite numbers fail
    return "# Capture card: " + c.id + "\n\n```card-json\n" + text +
           (text.empty() || text.back() != '\n' ? "\n" : "") + "```\n";
}

}  // namespace sonora::ampcab
