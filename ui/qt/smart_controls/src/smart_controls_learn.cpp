// SPDX-License-Identifier: GPL-3.0-or-later
//
// Learn mode and the mapping list: what a touch makes, and the edits the
// panel makes on a mapping (invert, range, remove).
#include <QVariantMap>
#include <algorithm>

#include "smart_controls_model.h"

namespace sonora::ui {

namespace sc = smart_controls;

InsertController::ParamSender SmartControlsModel::learnTap(InsertController::ParamSender downstream) {
    return [this, downstream = std::move(downstream)](model::Id insert, const std::string& key,
                                                      float value) {
        if (downstream) downstream(insert, key, value);
        touched(insert, key, value);
    };
}

void SmartControlsModel::learn(int macro) {
    if (macro < 0 || macro >= static_cast<int>(set_.macros.size())) return;
    if (learning_ == macro) {
        finishLearn();
        return;
    }
    if (learning_ >= 0) finishLearn();
    learning_ = macro;
    selected_ = macro;
    emit changed();
}

void SmartControlsModel::touched(model::Id insert, const std::string& key, float value) {
    if (applying_ || learning_ < 0) return;
    sc::Macro& macro = set_.macros[static_cast<std::size_t>(learning_)];
    if (capturing_) {
        if (insert == capture_insert_ && key == capture_key_) {
            for (sc::Mapping& m : macro.mappings) {
                if (m.insert == insert && m.key == key) m.hi = value;
            }
            emit changed();
            return;
        }
        // The next touched parameter was the one; another one ends learn.
        finishLearn();
        return;
    }
    const api::PluginParam* p = find_param(ctl_, insert, key);
    if (p == nullptr) return;   // the controller refused it before sending
    sc::Mapping m;
    m.insert = insert;
    m.key = key;
    // The document still holds the value from before the touch: the
    // controller keeps a running gesture's values apart until it ends.
    m.lo = document_value(insert, *p);
    m.hi = value;
    std::string why;
    macro.mappings.push_back(m);
    why = sc::validate(set_);
    if (!why.empty()) {
        macro.mappings.pop_back();
        learning_ = -1;
        refuse("Learn did not map " + p->name + ": " + why);
        return;
    }
    capturing_ = true;
    capture_insert_ = insert;
    capture_key_ = key;
    emit changed();
}

void SmartControlsModel::finishLearn() {
    if (learning_ < 0) return;
    if (capturing_) {
        sc::Macro& macro = set_.macros[static_cast<std::size_t>(learning_)];
        for (sc::Mapping& m : macro.mappings) {
            if (m.insert != capture_insert_ || m.key != capture_key_ || m.lo != m.hi) continue;
            // A click without travel: the whole range, not a knob that moves nothing.
            if (const api::PluginParam* p = find_param(ctl_, m.insert, m.key)) {
                m.lo = p->min;
                m.hi = p->max;
            }
        }
        save();
    }
    learning_ = -1;
    capturing_ = false;
    capture_insert_ = 0;
    capture_key_.clear();
    emit changed();
}

bool SmartControlsModel::addMapping(int macro, const sc::Mapping& m, std::string& why) {
    if (macro < 0 || macro >= static_cast<int>(set_.macros.size())) {
        why = "no such macro";
        return false;
    }
    auto& list = set_.macros[static_cast<std::size_t>(macro)].mappings;
    list.push_back(m);
    why = sc::validate(set_);
    if (!why.empty()) {
        list.pop_back();
        return false;
    }
    save();
    emit changed();
    return true;
}

sc::Mapping* SmartControlsModel::selected_mapping(int mapping) {
    auto& list = set_.macros[static_cast<std::size_t>(selected_)].mappings;
    if (mapping < 0 || mapping >= static_cast<int>(list.size())) return nullptr;
    return &list[static_cast<std::size_t>(mapping)];
}

void SmartControlsModel::setInvert(int mapping, bool invert) {
    sc::Mapping* m = selected_mapping(mapping);
    if (m == nullptr || m->invert == invert) return;
    m->invert = invert;
    save();
    emit changed();
}

// Positions along the parameter's own range; a mapping whose parameter is
// unknown has no range to place them on, and keeps its values.
void SmartControlsModel::setRangePosition(int mapping, double lo, double hi) {
    sc::Mapping* m = selected_mapping(mapping);
    if (m == nullptr) return;
    const api::PluginParam* p = find_param(ctl_, m->insert, m->key);
    if (p == nullptr) {
        refuse("That mapping's parameter is not in this session, so its range cannot be set");
        return;
    }
    m->lo = param_value_at(*p, static_cast<float>(std::clamp(lo, 0.0, 1.0)));
    m->hi = param_value_at(*p, static_cast<float>(std::clamp(hi, 0.0, 1.0)));
    save();
    emit changed();
}

void SmartControlsModel::removeMapping(int mapping) {
    auto& list = set_.macros[static_cast<std::size_t>(selected_)].mappings;
    if (mapping < 0 || mapping >= static_cast<int>(list.size())) return;
    const sc::Mapping gone = list[static_cast<std::size_t>(mapping)];
    list.erase(list.begin() + mapping);
    if (capturing_ && gone.insert == capture_insert_ && gone.key == capture_key_) {
        capturing_ = false;
        learning_ = -1;
    }
    save();
    emit changed();
}

QVariantMap SmartControlsModel::mapping_view(const sc::Mapping& m, bool capturing) const {
    QVariantMap row;
    row[QStringLiteral("invert")] = m.invert;
    row[QStringLiteral("capturing")] = capturing;
    const api::PluginInfo* plugin = ctl_.plugin_of(m.insert);
    const api::PluginParam* p = find_param(ctl_, m.insert, m.key);
    int track = -1, slot = -1;
    const bool located = ctl_.locate(m.insert, track, slot);
    row[QStringLiteral("unresolved")] = p == nullptr;
    if (p == nullptr) {
        row[QStringLiteral("plugin")] = located && plugin == nullptr
                                            ? QStringLiteral("Plug-in not installed")
                                            : QStringLiteral("Insert not in this session");
        row[QStringLiteral("param")] = QString::fromStdString(m.key);
        row[QStringLiteral("loPos")] = 0.0;
        row[QStringLiteral("hiPos")] = 1.0;
        row[QStringLiteral("loText")] = QString::number(static_cast<double>(m.lo), 'g', 4);
        row[QStringLiteral("hiText")] = QString::number(static_cast<double>(m.hi), 'g', 4);
        row[QStringLiteral("label")] = QStringLiteral("not found");
        return row;
    }
    const auto& tracks = ctl_.snapshot().edit->tracks;
    QString where;
    if (located && track >= 0 && track < static_cast<int>(tracks.size()))
        where = QStringLiteral("%1 · %2").arg(QString::fromStdString(tracks[static_cast<std::size_t>(track)]->name),
                                              QString::fromStdString(slot_letter(slot)));
    row[QStringLiteral("label")] = where;
    row[QStringLiteral("plugin")] = QString::fromStdString(plugin->name);
    row[QStringLiteral("param")] = QString::fromStdString(p->name);
    row[QStringLiteral("loPos")] = param_position(*p, m.lo);
    row[QStringLiteral("hiPos")] = param_position(*p, m.hi);
    row[QStringLiteral("loText")] = QString::fromStdString(param_text(*plugin, *p, m.lo));
    row[QStringLiteral("hiText")] = QString::fromStdString(param_text(*plugin, *p, m.hi));
    return row;
}

QVariantList SmartControlsModel::mappings() const {
    QVariantList out;
    const sc::Macro& macro = set_.macros[static_cast<std::size_t>(selected_)];
    for (const sc::Mapping& m : macro.mappings) {
        const bool capturing = capturing_ && learning_ == selected_ && m.insert == capture_insert_ &&
                               m.key == capture_key_;
        out.push_back(mapping_view(m, capturing));
    }
    return out;
}

}  // namespace sonora::ui
