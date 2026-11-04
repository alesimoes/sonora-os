// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/model/plan.h"

#include <algorithm>
#include <cstdio>

namespace sonora::model {

namespace {

std::string num(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", static_cast<double>(v));
    return buf;
}

const char* yes(bool v) { return v ? "on" : "off"; }

const TrackEdit* find_track(const EditState& e, Id id) {
    for (const auto& t : e.tracks)
        if (t->id == id) return t.get();
    return nullptr;
}

const Bus* find_bus(const EditState& e, Id id) {
    for (const Bus& b : e.buses)
        if (b.id == id) return &b;
    return nullptr;
}

std::string bus_name(const Snapshot& s, Id id) {
    const Bus* b = find_bus(*s.edit, id);
    return b != nullptr ? b->name : "bus " + std::to_string(id);
}

std::string slot_letter(std::uint8_t slot) { return std::string(1, static_cast<char>('A' + slot)); }

struct Emitter {
    std::size_t step;
    std::vector<DiffLine>& out;
    void added(const std::string& subject, const std::string& field, const std::string& after) {
        out.push_back({step, DiffKind::Added, subject, field, {}, after});
    }
    void removed(const std::string& subject, const std::string& field, const std::string& before) {
        out.push_back({step, DiffKind::Removed, subject, field, before, {}});
    }
    void changed(const std::string& subject, const std::string& field, const std::string& before,
                 const std::string& after) {
        out.push_back({step, DiffKind::Changed, subject, field, before, after});
    }
    void num_changed(const std::string& subject, const char* field, float a, float b) {
        if (a != b) changed(subject, field, num(a), num(b));
    }
    void flag_changed(const std::string& subject, const char* field, bool a, bool b) {
        if (a != b) changed(subject, field, yes(a), yes(b));
    }
};

const Send* send_by_id(const std::vector<Send>& v, Id id) {
    for (const Send& s : v)
        if (s.id == id) return &s;
    return nullptr;
}

// Sends and the values of those on both sides, for one strip.
void diff_sends(Emitter& e, const Snapshot& a, const Snapshot& b, const std::string& subject,
                const std::vector<Send>& sa, const std::vector<Send>& sb) {
    for (const Send& s : sb) {
        const Send* before = send_by_id(sa, s.id);
        const std::string name = subject + " send " + slot_letter(s.slot);
        if (before == nullptr) {
            e.added(subject, "send", slot_letter(s.slot) + " -> " + bus_name(b, s.bus));
            continue;
        }
        const SendMix* ma = a.mix.find_send(s.id);
        const SendMix* mb = b.mix.find_send(s.id);
        if (ma == nullptr || mb == nullptr) continue;
        e.num_changed(name, "level_db", ma->level_db, mb->level_db);
        e.num_changed(name, "pan", ma->pan, mb->pan);
        e.flag_changed(name, "mute", ma->mute, mb->mute);
        e.flag_changed(name, "pre", ma->pre, mb->pre);
        e.flag_changed(name, "follow_pan", ma->follow_pan, mb->follow_pan);
    }
    for (const Send& s : sa) {
        if (send_by_id(sb, s.id) == nullptr)
            e.removed(subject, "send", slot_letter(s.slot) + " -> " + bus_name(a, s.bus));
    }
}

void diff_inserts(Emitter& e, const Snapshot& a, const Snapshot& b, const std::string& subject,
                  const std::vector<Insert>& ia, const std::vector<Insert>& ib) {
    const auto find = [](const std::vector<Insert>& v, Id id) -> const Insert* {
        for (const Insert& i : v)
            if (i.id == id) return &i;
        return nullptr;
    };
    for (const Insert& i : ib) {
        const Insert* before = find(ia, i.id);
        if (before == nullptr) {
            e.added(subject, "insert", i.plugin.name + " (slot " + std::to_string(i.slot) + ")");
            continue;
        }
        const std::string name = subject + " insert " + i.plugin.name;
        e.flag_changed(name, "active", before->active, i.active);
        const InsertMix* ma = a.mix.find_insert(i.id);
        const InsertMix* mb = b.mix.find_insert(i.id);
        if (ma == nullptr || mb == nullptr) continue;
        e.flag_changed(name, "bypass", ma->bypass, mb->bypass);
        for (const ParamValue& p : mb->params) {
            const auto it = std::find_if(ma->params.begin(), ma->params.end(),
                                         [&](const ParamValue& q) { return q.key == p.key; });
            if (it == ma->params.end())
                e.added(name, p.key, num(p.value));
            else
                e.num_changed(name, p.key.c_str(), it->value, p.value);
        }
    }
    for (const Insert& i : ia) {
        if (find(ib, i.id) == nullptr)
            e.removed(subject, "insert", i.plugin.name + " (slot " + std::to_string(i.slot) + ")");
    }
}

std::string join_error(std::size_t step, const PlanStep& s, const std::string& why) {
    return "step " + std::to_string(step + 1) + " (" + s.title + "): " + why;
}

Compound compound_of(const Plan& plan, const Resolved& r) {
    Compound c;
    c.label = plan.label;
    for (const std::size_t i : r.applied) c.ops.push_back(plan.steps[i].op);
    return c;
}

}  // namespace

bool plan_revertible(const Member& m) {
    return member_is_undoable(m) || std::holds_alternative<SetMix>(m) ||
           std::holds_alternative<SetSendMix>(m) || std::holds_alternative<SetBusMix>(m);
}

std::string plan_problem(const Plan& plan) {
    if (plan.steps.empty()) return "the plan has no steps";
    if (plan.steps.size() > kMaxCompoundMembers)
        return "the plan has " + std::to_string(plan.steps.size()) + " steps; at most " +
               std::to_string(kMaxCompoundMembers);
    for (std::size_t i = 0; i < plan.steps.size(); ++i) {
        for (const std::size_t j : plan.steps[i].needs) {
            if (j >= i) return join_error(i, plan.steps[i], "needs a step that is not earlier");
        }
        if (!plan_revertible(plan.steps[i].op))
            return join_error(i, plan.steps[i], "a plan cannot take this operation back");
    }
    return {};
}

Selection Selection::all(const Plan& plan) {
    Selection s;
    s.accepted.assign(plan.steps.size(), true);
    return s;
}

Resolved resolve(const Plan& plan, const Selection& sel) {
    Resolved r;
    std::vector<bool> applied(plan.steps.size(), false);
    for (std::size_t i = 0; i < plan.steps.size(); ++i) {
        if (i >= sel.accepted.size() || !sel.accepted[i]) continue;
        bool ok = true;
        for (const std::size_t j : plan.steps[i].needs) ok = ok && j < i && applied[j];
        if (ok) {
            applied[i] = true;
            r.applied.push_back(i);
        } else {
            r.dropped.push_back(i);
        }
    }
    return r;
}

void diff_snapshots(const Snapshot& a, const Snapshot& b, std::size_t step,
                    std::vector<DiffLine>& out) {
    Emitter e{step, out};
    for (const auto& tb : b.edit->tracks) {
        const std::string subject = "Track " + tb->name;
        const TrackEdit* ta = find_track(*a.edit, tb->id);
        if (ta == nullptr) {
            e.added(subject, {}, {});
            continue;
        }
        if (ta->name != tb->name) e.changed(subject, "name", ta->name, tb->name);
        if (ta->output != tb->output)
            e.changed(subject, "output", bus_name(a, ta->output), bus_name(b, tb->output));
        diff_sends(e, a, b, subject, ta->sends, tb->sends);
        diff_inserts(e, a, b, subject, ta->inserts, tb->inserts);
        const TrackMix* ma = a.mix.find(tb->id);
        const TrackMix* mb = b.mix.find(tb->id);
        if (ma != nullptr && mb != nullptr) {
            e.num_changed(subject, "gain_db", ma->gain_db, mb->gain_db);
            e.num_changed(subject, "pan", ma->pan, mb->pan);
            e.flag_changed(subject, "mute", ma->mute, mb->mute);
            e.flag_changed(subject, "solo", ma->solo, mb->solo);
        }
    }
    for (const auto& ta : a.edit->tracks) {
        if (find_track(*b.edit, ta->id) == nullptr) e.removed("Track " + ta->name, {}, {});
    }
    for (const Bus& bb : b.edit->buses) {
        const std::string subject = "Bus " + bb.name;
        const Bus* ba = find_bus(*a.edit, bb.id);
        if (ba == nullptr) {
            e.added(subject, {}, {});
            continue;
        }
        if (ba->name != bb.name) e.changed(subject, "name", ba->name, bb.name);
        diff_sends(e, a, b, subject, ba->sends, bb.sends);
        const BusMix* ma = a.mix.find_bus(bb.id);
        const BusMix* mb = b.mix.find_bus(bb.id);
        if (ma != nullptr && mb != nullptr) {
            e.num_changed(subject, "gain_db", ma->gain_db, mb->gain_db);
            e.num_changed(subject, "pan", ma->pan, mb->pan);
            e.flag_changed(subject, "mute", ma->mute, mb->mute);
            e.flag_changed(subject, "solo", ma->solo, mb->solo);
        }
    }
    for (const Bus& ba : a.edit->buses) {
        if (find_bus(*b.edit, ba.id) == nullptr) e.removed("Bus " + ba.name, {}, {});
    }
}

bool preview(const Snapshot& at, const Plan& plan, const Selection& sel, PlanPreview& out,
             std::string& error, const MemberCheck& check) {
    error = plan_problem(plan);
    if (!error.empty()) return false;
    if (plan.base_revision != at.revision) {
        error = "the session changed since the plan was proposed (revision " +
                std::to_string(plan.base_revision) + ", now " + std::to_string(at.revision) + ")";
        return false;
    }
    PlanPreview p;
    p.resolved = resolve(plan, sel);
    // Step by step for the diff and for errors that name the step; the
    // result itself is the one compound accept_plan() applies.
    Snapshot state = at;
    for (const std::size_t k : p.resolved.applied) {
        Snapshot next;
        std::string why;
        if (!apply(state, to_operation(plan.steps[k].op), next, why, check)) {
            error = join_error(k, plan.steps[k], why);
            return false;
        }
        diff_snapshots(state, next, k, p.lines);
        state = std::move(next);
    }
    p.result = at;
    if (!p.resolved.applied.empty() &&
        !apply(at, Operation(compound_of(plan, p.resolved)), p.result, error, check))
        return false;
    out = std::move(p);
    return true;
}

bool accept_plan(History& history, const Plan& plan, const Selection& sel, PlanReceipt& receipt,
                 std::string& error, const MemberCheck& check) {
    PlanPreview p;
    if (!preview(history.current(), plan, sel, p, error, check)) return false;
    PlanReceipt r;
    r.resolved = p.resolved;
    if (r.resolved.applied.empty()) {
        r.revision = history.current().revision;
        receipt = std::move(r);
        return true;
    }
    const Snapshot& before = history.current();
    r.restore.label = "Undo " + plan.label;
    const auto first = [&](const auto& pred) {
        return std::none_of(r.restore.ops.begin(), r.restore.ops.end(), pred);
    };
    for (const std::size_t k : r.resolved.applied) {
        const Member& m = plan.steps[k].op;
        if (const auto* s = std::get_if<SetMix>(&m)) {
            const TrackMix* old = before.mix.find(s->mix.track);
            const Id id = s->mix.track;
            if (old != nullptr && first([&](const Member& x) {
                    const auto* y = std::get_if<SetMix>(&x);
                    return y != nullptr && y->mix.track == id;
                }))
                r.restore.ops.push_back(SetMix{*old});
        } else if (const auto* s2 = std::get_if<SetSendMix>(&m)) {
            const SendMix* old = before.mix.find_send(s2->mix.send);
            const Id id = s2->mix.send;
            if (old != nullptr && first([&](const Member& x) {
                    const auto* y = std::get_if<SetSendMix>(&x);
                    return y != nullptr && y->mix.send == id;
                }))
                r.restore.ops.push_back(SetSendMix{*old});
        } else if (const auto* s3 = std::get_if<SetBusMix>(&m)) {
            const BusMix* old = before.mix.find_bus(s3->mix.bus);
            const Id id = s3->mix.bus;
            if (old != nullptr && first([&](const Member& x) {
                    const auto* y = std::get_if<SetBusMix>(&x);
                    return y != nullptr && y->mix.bus == id;
                }))
                r.restore.ops.push_back(SetBusMix{*old});
        }
    }
    const Compound c = compound_of(plan, r.resolved);
    if (!history.apply(c, error, check)) return false;
    r.revision = history.current().revision;
    r.pushed_step = is_undoable(c);
    receipt = std::move(r);
    return true;
}

bool undo_plan(History& history, const PlanReceipt& receipt, std::string& error) {
    if (receipt.resolved.applied.empty()) return true;
    if (history.current().revision != receipt.revision) {
        error = "the plan is no longer the latest change; undo what came after it first";
        return false;
    }
    if (receipt.pushed_step && !history.undo()) {
        error = "nothing to undo";
        return false;
    }
    if (!receipt.restore.ops.empty() && !history.apply(receipt.restore, error)) return false;
    return true;
}

}  // namespace sonora::model
