// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/state.h"

#include <cstdint>
#include <vector>

#include "sonora/json/json.h"

namespace sonora::native {

namespace {

using json::Value;

const char* unit_name(Unit u) {
    switch (u) {
        case Unit::None: return "none";
        case Unit::Decibel: return "dB";
        case Unit::Hertz: return "Hz";
        case Unit::Milliseconds: return "ms";
        case Unit::Percent: return "%";
        case Unit::Ratio: return "ratio";
        case Unit::Q: return "Q";
        case Unit::Bpm: return "BPM";
        case Unit::Choice: return "choice";
        case Unit::Toggle: return "toggle";
        case Unit::Seconds: return "s";
    }
    return "none";
}

const char* taper_name(Taper t) {
    switch (t) {
        case Taper::Linear: return "linear";
        case Taper::Log: return "log";
        case Taper::Stepped: return "stepped";
    }
    return "linear";
}

std::string written(const Value& v) {
    std::string out, err;
    json::write(v, out, err);
    return out;
}

}  // namespace

std::string descriptor_json(const Descriptor& d, const FormatSymbol* symbols,
                            std::uint32_t symbol_count) {
    Value root = Value::object();
    root.set("id", Value::string(d.id));
    root.set("name", Value::string(d.name));
    root.set("category", Value::string(d.category));
    root.set("state_version", Value::integer(d.state_version));
    root.set("min_channels", Value::integer(d.min_channels));
    root.set("max_channels", Value::integer(d.max_channels));
    root.set("sidechain", Value::boolean(d.sidechain));
    Value params = Value::array();
    for (std::uint32_t i = 0; i < d.param_count; ++i) {
        const ParamInfo& p = d.params[i];
        Value o = Value::object();
        o.set("id", Value::string(p.id));
        o.set("index", Value::integer(i));
        o.set("name", Value::string(p.name));
        o.set("unit", Value::string(unit_name(p.unit)));
        o.set("taper", Value::string(taper_name(p.taper)));
        o.set("min", Value::number_float(p.min));
        o.set("max", Value::number_float(p.max));
        o.set("default", Value::number_float(p.def));
        o.set("smoothing_ms", Value::number_float(p.smoothing_ms));
        o.set("automatable", Value::boolean(p.automatable));
        o.set("reconfigures", Value::boolean(p.reconfigures));
        o.set("format", Value::string(p.format));
        if (p.choice_count > 0) {
            Value c = Value::array();
            for (std::uint32_t k = 0; k < p.choice_count; ++k) c.push(Value::string(p.choices[k]));
            o.set("choices", std::move(c));
        }
        Value formats = Value::object();
        bool any = false;
        for (std::uint32_t k = 0; k < symbol_count; ++k) {
            if (symbols[k].index != i) continue;
            Value f = Value::object();
            f.set("symbol", Value::string(symbols[k].symbol));
            f.set("inverted", Value::boolean(symbols[k].inverted));
            formats.set(symbols[k].format, std::move(f));
            any = true;
        }
        if (any) o.set("formats", std::move(formats));
        params.push(std::move(o));
    }
    root.set("params", std::move(params));
    return written(root);
}

std::string save_state(const Processor& p) {
    const Descriptor& d = p.descriptor();
    Value root = Value::object();
    root.set("processor", Value::string(d.id));
    root.set("state_version", Value::integer(d.state_version));
    Value params = Value::object();
    for (std::uint32_t i = 0; i < d.param_count; ++i) {
        params.set(d.params[i].id, Value::number_float(p.param(i)));
    }
    root.set("params", std::move(params));
    return written(root);
}

bool load_state(Processor& p, const std::string& text, std::string& error) {
    const Descriptor& d = p.descriptor();
    Value root;
    json::Error je;
    if (!json::parse(text, root, je)) {
        error = "state is not valid JSON: " + je.message + " at line " + std::to_string(je.line);
        return false;
    }
    const Value* proc = root.find("processor");
    if (proc == nullptr || !proc->is_string() || proc->text() != d.id) {
        error = std::string("state is not for ") + d.id;
        return false;
    }
    const Value* ver = root.find("state_version");
    std::int64_t version = 0;
    if (ver == nullptr || !ver->to_int64(version) || version < 1) {
        error = "state has no valid state_version";
        return false;
    }
    if (version > d.state_version) {
        error = "state version " + std::to_string(version) + " is newer than this processor's " +
                std::to_string(d.state_version);
        return false;
    }
    // Version 1 is the only one yet; a later version adds its migration
    // here, with a test that loads the previous version's golden state.
    const Value* params = root.find("params");
    if (params == nullptr || !params->is_object()) {
        error = "state has no params object";
        return false;
    }
    std::vector<float> values(d.param_count);
    for (std::uint32_t i = 0; i < d.param_count; ++i) values[i] = d.params[i].def;
    for (const auto& kv : params->members()) {
        std::uint32_t idx = d.param_count;
        for (std::uint32_t i = 0; i < d.param_count; ++i) {
            if (kv.first == d.params[i].id) idx = i;
        }
        if (idx == d.param_count) {
            error = "unknown parameter '" + kv.first + "' for " + d.id;
            return false;
        }
        float v = 0.0f;
        if (!kv.second.is_number() || !kv.second.to_float(v)) {
            error = "parameter '" + kv.first + "' is not a number";
            return false;
        }
        values[idx] = sanitize(d.params[idx], v);
    }
    for (std::uint32_t i = 0; i < d.param_count; ++i) p.set_param(i, values[i]);
    return true;
}

}  // namespace sonora::native
