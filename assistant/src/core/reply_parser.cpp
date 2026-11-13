// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/assistant/reply_parser.h"

#include <cmath>
#include <cstdint>
#include <string_view>

#include "sonora/json/json.h"

namespace sonora::assistant {
namespace {

using json::Value;

constexpr std::size_t kMaxText = 300;

struct Ctx {
    const FeatureBundle& bundle;
    const model::Snapshot& at;
    const model::Plan& plan;   // steps parsed so far, so two steps compose
    std::string err;
};

bool fail(Ctx& c, std::size_t i, const std::string& why) {
    c.err = "step " + std::to_string(i + 1) + ": " + why;
    return false;
}

bool read_float(const Value& step, const char* key, float lo, float hi, float& out) {
    const Value* v = step.find(key);
    if (!v || !v->is_number() || !v->to_float(out)) return false;
    return std::isfinite(out) && out >= lo && out <= hi;
}

bool read_id(const Value& step, const char* key, std::uint64_t& out) {
    const Value* v = step.find(key);
    std::int64_t n = 0;
    if (!v || !v->is_number() || !v->to_int64(n) || n <= 0) return false;
    out = static_cast<std::uint64_t>(n);
    return true;
}

bool read_text(const Value& step, const char* key, std::string& out) {
    const Value* v = step.find(key);
    if (!v) return true;   // optional
    if (!v->is_string() || v->text().size() > kMaxText || !json::valid_utf8(v->text()))
        return false;
    out = v->text();
    return true;
}

// The latest value a step earlier in the plan left, else the snapshot's.
template <class Op, class Mix, class Field>
const Mix* latest(const model::Plan& plan, Field field, std::uint64_t id) {
    const Mix* found = nullptr;
    for (const auto& s : plan.steps)
        if (const Op* op = std::get_if<Op>(&s.op))
            if ((op->mix.*field) == id) found = &op->mix;
    return found;
}

template <class Mix, class Vec, class Field>
const Mix* in_snapshot(const Vec& v, Field field, std::uint64_t id) {
    for (const auto& m : v)
        if (m.*field == id) return &m;
    return nullptr;
}

bool in_bundle_track(const FeatureBundle& b, std::uint64_t id) {
    for (const auto& t : b.tracks) if (t.track_id == id) return true;
    return false;
}
bool in_bundle_send(const FeatureBundle& b, std::uint64_t id) {
    for (const auto& t : b.tracks)
        for (const auto& s : t.sends) if (s.send_id == id) return true;
    return false;
}
bool in_bundle_bus(const FeatureBundle& b, std::uint64_t id) {
    for (const auto& x : b.buses) if (x.bus_id == id) return true;
    return false;
}

bool track_step(Ctx& c, const Value& s, std::size_t i, bool gain, model::PlanStep& out) {
    std::uint64_t id = 0;
    if (!read_id(s, "track", id) || !in_bundle_track(c.bundle, id))
        return fail(c, i, "track id is not in the session description");
    const model::TrackMix* base = latest<model::SetMix, model::TrackMix>(
        c.plan, &model::TrackMix::track, id);
    if (!base) base = in_snapshot<model::TrackMix>(c.at.mix.tracks, &model::TrackMix::track, id);
    if (!base) return fail(c, i, "track is not in the snapshot");
    model::TrackMix m = *base;
    if (gain) {
        if (!read_float(s, "gain_db", model::kMinGainDb, model::kMaxGainDb, m.gain_db))
            return fail(c, i, "gain_db missing or out of range");
    } else if (!read_float(s, "pan", -1.0f, 1.0f, m.pan)) {
        return fail(c, i, "pan missing or outside -1..1");
    }
    out.op = model::SetMix{m};
    return true;
}

bool send_step(Ctx& c, const Value& s, std::size_t i, model::PlanStep& out) {
    std::uint64_t id = 0;
    if (!read_id(s, "send", id) || !in_bundle_send(c.bundle, id))
        return fail(c, i, "send id is not in the session description");
    const model::SendMix* base = latest<model::SetSendMix, model::SendMix>(
        c.plan, &model::SendMix::send, id);
    if (!base) base = in_snapshot<model::SendMix>(c.at.mix.sends, &model::SendMix::send, id);
    if (!base) return fail(c, i, "send is not in the snapshot");
    model::SendMix m = *base;
    if (!read_float(s, "level_db", model::kMinGainDb, model::kMaxGainDb, m.level_db))
        return fail(c, i, "level_db missing or out of range");
    out.op = model::SetSendMix{m};
    return true;
}

bool bus_step(Ctx& c, const Value& s, std::size_t i, model::PlanStep& out) {
    std::uint64_t id = 0;
    if (!read_id(s, "bus", id) || !in_bundle_bus(c.bundle, id))
        return fail(c, i, "bus id is not in the session description");
    const model::BusMix* base = latest<model::SetBusMix, model::BusMix>(
        c.plan, &model::BusMix::bus, id);
    if (!base) base = in_snapshot<model::BusMix>(c.at.mix.buses, &model::BusMix::bus, id);
    if (!base) return fail(c, i, "bus is not in the snapshot");
    model::BusMix m = *base;
    if (!read_float(s, "gain_db", model::kMinGainDb, model::kMaxGainDb, m.gain_db))
        return fail(c, i, "gain_db missing or out of range");
    out.op = model::SetBusMix{m};
    return true;
}

bool one_step(Ctx& c, const Value& s, std::size_t i, model::PlanStep& out) {
    if (!s.is_object()) return fail(c, i, "not an object");
    const Value* op = s.find("op");
    if (!op || !op->is_string()) return fail(c, i, "no op");
    const std::string& name = op->text();
    bool ok = false;
    if (name == "set_track_gain") ok = track_step(c, s, i, true, out);
    else if (name == "set_track_pan") ok = track_step(c, s, i, false, out);
    else if (name == "set_send_level") ok = send_step(c, s, i, out);
    else if (name == "set_bus_gain") ok = bus_step(c, s, i, out);
    else return fail(c, i, "unknown op '" + name.substr(0, 40) + "'");
    if (!ok) return false;
    if (!read_text(s, "title", out.title) || !read_text(s, "reason", out.reason))
        return fail(c, i, "title or reason is not text of a sane length");
    if (out.title.empty()) out.title = name;
    return true;
}

}  // namespace

bool parse_reply(const std::string& body, const FeatureBundle& bundle,
                 const model::Snapshot& at, model::Plan& out, std::string& error) {
    out = model::Plan{};
    json::Value root;
    json::Error je;
    if (!json::parse(body, root, je)) {
        error = "reply is not JSON: " + je.message;
        return false;
    }
    const Value* steps = root.is_object() ? root.find("steps") : nullptr;
    if (!steps || !steps->is_array()) {
        error = "reply has no steps array";
        return false;
    }
    model::Plan plan;
    plan.label = "Claude: " + bundle.intent.substr(0, kMaxText);
    plan.base_revision = bundle.base_revision;
    Ctx c{bundle, at, plan, {}};
    for (std::size_t i = 0; i < steps->items().size(); ++i) {
        model::PlanStep step;
        if (!one_step(c, steps->items()[i], i, step)) {
            error = c.err;
            return false;
        }
        plan.steps.push_back(std::move(step));
    }
    const std::string problem = model::plan_problem(plan);
    if (!problem.empty()) {
        error = problem;
        return false;
    }
    out = std::move(plan);
    return true;
}

}  // namespace sonora::assistant
