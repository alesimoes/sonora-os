// SPDX-License-Identifier: GPL-3.0-or-later
#include "insert_controller.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>

namespace sonora::ui {

using namespace model;

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

PluginFormat model_format(api::PluginFormat f) {
    return f == api::PluginFormat::Vst3 ? PluginFormat::Vst3 : PluginFormat::Lv2;
}

PluginRef ref_of(const api::PluginInfo& p) {
    PluginRef r;
    r.format = model_format(p.format);
    r.id = p.id;
    r.name = p.name;
    r.version = p.version;
    return r;
}

// A toggle with no step count still takes two values.
uint32_t effective_steps(const api::PluginParam& p) {
    if (p.steps >= 2) return p.steps;
    return (p.flags & api::kParamToggle) != 0 ? 2 : 0;
}

bool logarithmic(const api::PluginParam& p) {
    return (p.flags & api::kParamLogarithmic) != 0 && p.min > 0.0f && p.max > p.min;
}

std::string number(float v) {
    const float a = std::fabs(v);
    const int decimals = a >= 100.0f ? 0 : a >= 10.0f ? 1 : a >= 1.0f ? 2 : 3;
    // Never "-0.0": a value that rounds to zero reads as zero.
    if (a < 0.5f * std::pow(10.0f, static_cast<float>(-decimals))) v = 0.0f;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, static_cast<double>(v));
    return buf;
}

std::string channels_text(uint32_t n) {
    return n == 1 ? "mono" : n == 2 ? "stereo" : std::to_string(n) + "-channel";
}

}  // namespace

std::string slot_letter(int slot) {
    if (slot < 0 || slot >= static_cast<int>(kInsertSlots)) return "?";
    return std::string(1, static_cast<char>('A' + slot));
}

// --- parameter values ------------------------------------------------------

int param_step(const api::PluginParam& p, float value) {
    const uint32_t steps = effective_steps(p);
    if (steps < 2) return -1;
    const float span = p.max - p.min;
    if (!(span > 0.0f)) return 0;
    const long i = std::lround((value - p.min) / span * static_cast<float>(steps - 1));
    return static_cast<int>(std::clamp<long>(i, 0, static_cast<long>(steps) - 1));
}

float param_step_value(const api::PluginParam& p, int i) {
    const uint32_t steps = effective_steps(p);
    if (steps < 2) return p.min;
    i = std::clamp(i, 0, static_cast<int>(steps) - 1);
    return p.min + (p.max - p.min) * static_cast<float>(i) / static_cast<float>(steps - 1);
}

float param_position(const api::PluginParam& p, float value) {
    const float span = p.max - p.min;
    if (!(span > 0.0f) || !std::isfinite(value)) return 0.0f;
    const uint32_t steps = effective_steps(p);
    if (steps >= 2) return static_cast<float>(param_step(p, value)) / static_cast<float>(steps - 1);
    value = std::clamp(value, p.min, p.max);
    if (logarithmic(p)) return std::log(value / p.min) / std::log(p.max / p.min);
    return (value - p.min) / span;
}

float param_value_at(const api::PluginParam& p, float position) {
    if (!std::isfinite(position)) position = 0.0f;
    position = std::clamp(position, 0.0f, 1.0f);
    const uint32_t steps = effective_steps(p);
    if (steps >= 2) {
        return param_step_value(p, static_cast<int>(std::lround(position * static_cast<float>(steps - 1))));
    }
    float v = logarithmic(p) ? p.min * std::pow(p.max / p.min, position)
                             : p.min + (p.max - p.min) * position;
    return std::clamp(v, p.min, p.max);
}

std::string param_text(const api::PluginInfo& plugin, const api::PluginParam& p, float value) {
    const uint32_t steps = effective_steps(p);
    if (steps >= 2) {
        const int i = param_step(p, value);
        if (p.labels.size() == steps) return p.labels[static_cast<std::size_t>(i)];
        if (steps == 2 && (p.flags & api::kParamToggle) != 0) return i == 0 ? "Off" : "On";
        if (plugin.format == api::PluginFormat::Vst3) {
            return std::to_string(i + 1) + " of " + std::to_string(steps);
        }
        return number(param_step_value(p, i)) + (p.unit.empty() ? "" : " " + p.unit);
    }
    if (plugin.format == api::PluginFormat::Vst3) {
        return std::to_string(std::lround(param_position(p, value) * 100.0f)) + " %";
    }
    return number(value) + (p.unit.empty() ? "" : " " + p.unit);
}

// --- controller ------------------------------------------------------------

InsertController::InsertController(TimelineDocument& doc) : doc_(doc) {}

const TrackEdit* InsertController::track(int index) const {
    const auto& ts = snapshot().edit->tracks;
    if (index < 0 || index >= static_cast<int>(ts.size())) return nullptr;
    return ts[static_cast<std::size_t>(index)].get();
}

const Insert* InsertController::insert_at(int t, int s) const {
    const TrackEdit* tr = track(t);
    if (tr == nullptr) return nullptr;
    for (const Insert& in : tr->inserts) {
        if (in.slot == s) return &in;
    }
    return nullptr;
}

const Insert* InsertController::find(Id insert, int* track_index) const {
    const auto& ts = snapshot().edit->tracks;
    for (std::size_t t = 0; t < ts.size(); ++t) {
        for (const Insert& in : ts[t]->inserts) {
            if (in.id != insert) continue;
            if (track_index != nullptr) *track_index = static_cast<int>(t);
            return &in;
        }
    }
    return nullptr;
}

bool InsertController::locate(Id insert, int& t, int& s) const {
    const Insert* in = find(insert, &t);
    if (in == nullptr) return false;
    s = in->slot;
    return true;
}

void InsertController::set_catalog(const api::PluginCatalog& c) {
    catalog_ = c;
    has_catalog_ = true;
}

const api::PluginInfo* InsertController::find_plugin(const PluginRef& ref) const {
    for (const api::PluginInfo& p : catalog_.plugins) {
        if (model_format(p.format) == ref.format && p.id == ref.id) return &p;
    }
    return nullptr;
}

bool InsertController::set_fault(Id insert, const std::string& text) {
    const auto it = faults_.find(insert);
    if (text.empty()) {
        if (it == faults_.end()) return false;
        faults_.erase(it);
        return true;
    }
    if (it != faults_.end() && it->second == text) return false;
    faults_[insert] = text;
    return true;
}

bool InsertController::set_frame(const EngineFrame& f, bool alive) {
    if (!alive) {
        const bool changed = alive_;
        alive_ = false;
        return changed;
    }
    // A torn read says nothing; the next one will.
    if (f.transport.stale) return false;
    std::vector<TrackState> now(f.tracks.size());
    for (std::size_t i = 0; i < f.tracks.size(); ++i) {
        now[i].latency = f.tracks[i].latency_frames;
        now[i].cells = f.tracks[i].inserts;
    }
    const bool disabled = (f.transport.health_flags & kHealthPluginsDisabled) != 0;
    const auto comp = static_cast<int64_t>(f.transport.compensation_frames);
    const auto short_by = static_cast<int64_t>(f.transport.compensation_shortfall_frames);
    const bool changed = !alive_ || now != tracks_ || disabled != plugins_disabled_ ||
                         comp != compensation_ || short_by != shortfall_;
    alive_ = true;
    tracks_ = std::move(now);
    plugins_disabled_ = disabled;
    compensation_ = comp;
    shortfall_ = short_by;
    return changed;
}

SlotView InsertController::slot(int t, int s) const {
    SlotView v;
    const Insert* in = insert_at(t, s);
    if (in == nullptr) return v;
    v.present = true;
    v.insert = in->id;
    v.name = in->plugin.name;
    v.active = in->active;
    if (const InsertMix* m = snapshot().mix.find_insert(in->id)) v.bypass = m->bypass;
    v.has_catalog_entry = !has_catalog_ || find_plugin(in->plugin) != nullptr;

    uint32_t flags = 0;
    // The block may not have caught up with an edit shown ahead of the
    // engine's answer; a slot it says is empty tells nothing about this one.
    if (alive_ && t < static_cast<int>(tracks_.size())) {
        const InsertSlotSnapshot& b = tracks_[static_cast<std::size_t>(t)].cells[static_cast<std::size_t>(s)];
        if ((b.flags & kSlotPresent) != 0) {
            flags = b.flags;
            v.latency_frames = b.latency_frames;
        }
    }
    v.running = (flags & kSlotRunning) != 0;
    v.restarting = (flags & kSlotRestarting) != 0;
    v.missing = (flags & kSlotMissing) != 0 || !v.has_catalog_entry;

    const auto fault = faults_.find(in->id);
    if (fault != faults_.end()) v.fault = fault->second;
    // An inactive insert is out of the chain: nothing of it can be down.
    if (!v.active) {
        v.fault.clear();
        return v;
    }
    v.faulted = !v.fault.empty() || v.missing || (flags & kSlotFaulted) != 0;
    if (v.faulted && v.fault.empty()) {
        if (v.missing) {
            v.fault = v.name + " is not installed here; the insert is kept, out of the chain";
        } else if (plugins_disabled_) {
            v.fault = "this engine hosts no plugins yet: the insert is kept and does not run";
        } else {
            v.fault = v.name + " is down; the engine has not said why yet";
        }
    }
    return v;
}

int64_t InsertController::track_latency(int t) const {
    if (!alive_ || t < 0 || t >= static_cast<int>(tracks_.size())) return -1;
    return tracks_[static_cast<std::size_t>(t)].latency;
}

// --- the chooser ---

std::vector<std::string> InsertController::categories() const {
    std::set<std::string> all;
    for (const api::PluginInfo& p : catalog_.plugins) {
        for (const std::string& c : p.categories) {
            if (!c.empty() && c != "Fx") all.insert(c);
        }
    }
    return {all.begin(), all.end()};
}

std::vector<int> InsertController::browse(const std::string& category,
                                          const std::string& search) const {
    const std::string needle = lower(search);
    std::vector<int> out;
    for (std::size_t i = 0; i < catalog_.plugins.size(); ++i) {
        const api::PluginInfo& p = catalog_.plugins[i];
        if (!category.empty() &&
            std::find(p.categories.begin(), p.categories.end(), category) == p.categories.end()) {
            continue;
        }
        if (!needle.empty() && lower(p.name).find(needle) == std::string::npos &&
            lower(p.vendor).find(needle) == std::string::npos) {
            continue;
        }
        out.push_back(static_cast<int>(i));
    }
    std::sort(out.begin(), out.end(), [this](int a, int b) {
        return lower(catalog_.plugins[static_cast<std::size_t>(a)].name) <
               lower(catalog_.plugins[static_cast<std::size_t>(b)].name);
    });
    return out;
}

std::string InsertController::refusal(int index, int t) const {
    if (index < 0 || index >= static_cast<int>(catalog_.plugins.size())) {
        return "no such plugin in the engine's catalog";
    }
    const api::PluginInfo& p = catalog_.plugins[static_cast<std::size_t>(index)];
    if (!p.usable) return p.name + " cannot be used: " + p.refusal;
    const TrackEdit* tr = track(t);
    if (tr == nullptr) return "there is no track " + std::to_string(t + 1);
    if (p.inputs != tr->channels || p.outputs != tr->channels) {
        return p.name + " is " + channels_text(p.inputs) +
               (p.outputs != p.inputs ? " in, " + channels_text(p.outputs) + " out" : "") +
               "; " + tr->name + " is " + channels_text(tr->channels);
    }
    return {};
}

// --- structure ---

bool InsertController::target_slot(int t, int s, std::string& why) const {
    const TrackEdit* tr = track(t);
    if (tr == nullptr) {
        why = "there is no track " + std::to_string(t + 1);
        return false;
    }
    if (s < 0 || s >= static_cast<int>(kInsertSlots)) {
        why = "inserts go in slots A to J";
        return false;
    }
    if (const Insert* there = insert_at(t, s)) {
        why = "insert " + slot_letter(s) + " on " + tr->name + " holds " + there->plugin.name +
              "; remove it first, or choose a free slot";
        return false;
    }
    return true;
}

bool InsertController::add(int t, int s, int plugin, std::string& why) {
    if (!target_slot(t, s, why)) return false;
    why = refusal(plugin, t);
    if (!why.empty()) return false;
    AddInsert op;
    op.track = track(t)->id;
    op.insert = doc_.reserve_id();
    op.slot = static_cast<uint8_t>(s);
    op.plugin = ref_of(catalog_.plugins[static_cast<std::size_t>(plugin)]);
    return doc_.submit(op, why);
}

bool InsertController::replace(int t, int s, int plugin, std::string& why) {
    const Insert* in = insert_at(t, s);
    if (in == nullptr) {
        why = "insert " + slot_letter(s) + " is empty; nothing to replace";
        return false;
    }
    why = refusal(plugin, t);
    if (!why.empty()) return false;
    ReplaceInsert op;
    op.insert = in->id;
    op.replacement = doc_.reserve_id();
    op.plugin = ref_of(catalog_.plugins[static_cast<std::size_t>(plugin)]);
    return doc_.submit(op, why);
}

bool InsertController::remove(int t, int s, std::string& why) {
    const Insert* in = insert_at(t, s);
    if (in == nullptr) {
        why = "insert " + slot_letter(s) + " is empty";
        return false;
    }
    if (gesture_insert_ == in->id) {
        gesture_insert_ = kNoId;
        gesture_.clear();
    }
    return doc_.submit(RemoveInsert{in->id}, why);
}

bool InsertController::move(int ft, int fs, int tt, int ts, std::string& why) {
    const Insert* in = insert_at(ft, fs);
    if (in == nullptr) {
        why = "insert " + slot_letter(fs) + " is empty; nothing to move";
        return false;
    }
    if (ft == tt && fs == ts) return true;
    if (!target_slot(tt, ts, why)) return false;
    const TrackEdit* from = track(ft);
    const TrackEdit* to = track(tt);
    if (from->channels != to->channels) {
        why = in->plugin.name + " runs " + channels_text(from->channels) + " on " + from->name +
              "; " + to->name + " is " + channels_text(to->channels) +
              ". Choose the plugin there instead";
        return false;
    }
    return doc_.submit(MoveInsert{in->id, to->id, static_cast<uint8_t>(ts)}, why);
}

void InsertController::copy(int ft, int fs, int tt, int ts, Done done) {
    std::string why;
    const Insert* in = insert_at(ft, fs);
    if (in == nullptr) {
        done(false, "insert " + slot_letter(fs) + " is empty; nothing to copy");
        return;
    }
    if (!target_slot(tt, ts, why)) {
        done(false, why);
        return;
    }
    if (track(ft)->channels != track(tt)->channels) {
        done(false, in->plugin.name + " runs " + channels_text(track(ft)->channels) + "; " +
                        track(tt)->name + " is " + channels_text(track(tt)->channels) +
                        ". Choose the plugin there instead");
        return;
    }
    const Id source = in->id;
    const Id to_track = track(tt)->id;
    doc_.flush_plugin_state(source, [this, source, to_track, ts, done](bool flushed,
                                                                        const std::string& fw) {
        // The document may have moved on while the engine answered: find
        // both ends again by id.
        int from_index = -1;
        const Insert* src = find(source, &from_index);
        const int to_index = track_index(snapshot(), to_track);
        std::string why;
        if (src == nullptr) {
            done(false, "the insert to copy is gone");
            return;
        }
        if (!target_slot(to_index, ts, why)) {
            done(false, why);
            return;
        }
        AddInsert op;
        op.track = to_track;
        op.insert = doc_.reserve_id();
        op.slot = static_cast<uint8_t>(ts);
        op.plugin = src->plugin;
        if (const InsertMix* m = snapshot().mix.find_insert(source)) {
            op.params = m->params;
            op.state = m->state;
        }
        const bool was_active = src->active;
        const bool was_bypassed = [&] {
            const InsertMix* m = snapshot().mix.find_insert(source);
            return m != nullptr && m->bypass;
        }();
        if (!doc_.submit(op, why)) {
            done(false, why);
            return;
        }
        // The copy is the insert as it was, inactive or bypassed included.
        // Not one undo step with the add (the model has no compound
        // operation yet); undo takes the add back and leaves nothing behind.
        if (!was_active) doc_.submit(SetInsertActive{op.insert, false}, why);
        if (was_bypassed) doc_.submit(SetInsertBypass{op.insert, true}, why);
        done(true, flushed ? std::string()
                           : "copied with its last saved state, as the plugin's own could not "
                             "be had: " + fw);
    });
}

bool InsertController::set_active(int t, int s, bool active, std::string& why) {
    const Insert* in = insert_at(t, s);
    if (in == nullptr) {
        why = "insert " + slot_letter(s) + " is empty";
        return false;
    }
    if (in->active == active) return true;
    return doc_.submit(SetInsertActive{in->id, active}, why);
}

bool InsertController::toggle_bypass(int t, int s, std::string& why) {
    const Insert* in = insert_at(t, s);
    if (in == nullptr) {
        why = "insert " + slot_letter(s) + " is empty; nothing to bypass";
        return false;
    }
    const InsertMix* m = snapshot().mix.find_insert(in->id);
    return doc_.submit(SetInsertBypass{in->id, !(m != nullptr && m->bypass)}, why);
}

// --- parameters ---

const api::PluginInfo* InsertController::plugin_of(Id insert) const {
    const Insert* in = find(insert);
    return in != nullptr ? find_plugin(in->plugin) : nullptr;
}

float InsertController::value(Id insert, const api::PluginParam& p) const {
    if (insert == gesture_insert_) {
        const auto it = gesture_.find(p.key);
        if (it != gesture_.end()) return it->second;
    }
    if (const InsertMix* m = snapshot().mix.find_insert(insert)) {
        for (const ParamValue& v : m->params) {
            if (v.key == p.key) return v.value;
        }
    }
    return p.default_value;
}

bool InsertController::param_begin(Id insert, std::string& why) {
    if (gesture_insert_ != kNoId && gesture_insert_ != insert && !param_end(why)) return false;
    if (find(insert) == nullptr) {
        why = "that insert is gone";
        return false;
    }
    gesture_insert_ = insert;
    return true;
}

bool InsertController::param_set(Id insert, const std::string& key, float v, std::string& why) {
    if (gesture_insert_ != insert && !param_begin(insert, why)) return false;
    const api::PluginInfo* plugin = plugin_of(insert);
    if (plugin == nullptr) {
        why = "the plugin is not in the engine's catalog, so its parameters are unknown";
        return false;
    }
    const api::PluginParam* p = nullptr;
    for (const api::PluginParam& each : plugin->params) {
        if (each.key == key) p = &each;
    }
    if (p == nullptr) {
        why = plugin->name + " has no parameter \"" + key + "\"";
        return false;
    }
    if (!std::isfinite(v)) {
        why = p->name + " must be a number";
        return false;
    }
    v = std::clamp(v, p->min, p->max);
    if (effective_steps(*p) >= 2) v = param_step_value(*p, param_step(*p, v));
    gesture_[key] = v;
    if (send_param_) send_param_(insert, key, v);
    return true;
}

bool InsertController::param_end(std::string& why) {
    if (gesture_insert_ == kNoId) return true;
    const Id insert = gesture_insert_;
    std::map<std::string, float> moved;
    moved.swap(gesture_);
    gesture_insert_ = kNoId;
    if (moved.empty() || find(insert) == nullptr) return true;
    SetInsertParams op;
    op.insert = insert;
    if (const InsertMix* m = snapshot().mix.find_insert(insert)) op.params = m->params;
    for (const auto& [key, v] : moved) {
        bool found = false;
        for (ParamValue& pv : op.params) {
            if (pv.key != key) continue;
            pv.value = v;
            found = true;
        }
        if (!found) op.params.push_back(ParamValue{key, v});
    }
    return doc_.submit(op, why);
}

}  // namespace sonora::ui
