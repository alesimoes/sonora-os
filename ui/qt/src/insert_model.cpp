// SPDX-License-Identifier: GPL-3.0-or-later
#include "insert_model.h"

#include <QLocale>

#include "session_model.h"

namespace sonora::ui {

using namespace model;

namespace {

QString q(const std::string& s) { return QString::fromStdString(s); }

QVariantMap slot_map(const SlotView& v, int slot) {
    QVariantMap m;
    m.insert(QStringLiteral("present"), v.present);
    m.insert(QStringLiteral("letter"), q(slot_letter(slot)));
    m.insert(QStringLiteral("name"), q(v.name));
    m.insert(QStringLiteral("active"), v.active);
    m.insert(QStringLiteral("bypass"), v.bypass);
    m.insert(QStringLiteral("faulted"), v.faulted);
    m.insert(QStringLiteral("missing"), v.missing);
    m.insert(QStringLiteral("restarting"), v.restarting);
    m.insert(QStringLiteral("running"), v.running);
    m.insert(QStringLiteral("fault"), q(v.fault));
    return m;
}

const char* format_name(api::PluginFormat f) {
    return f == api::PluginFormat::Vst3 ? "VST3" : "LV2";
}

QString channels_name(uint32_t n) {
    return n == 1 ? QStringLiteral("Mono") : n == 2 ? QStringLiteral("Stereo")
                                                    : QStringLiteral("%1 ch").arg(n);
}

}  // namespace

InsertModel::InsertModel(TimelineDocument& doc, SessionModel& session, QObject* parent)
    : QObject(parent), ctl_(doc), session_(session) {
    // Every document change reaches the windows through SessionModel's
    // signals; the slots follow the same ones.
    connect(&session_, &SessionModel::mixChanged, this, [this]() {
        refresh();
        refreshPanel();
    });
    refresh();
}

bool InsertModel::report(bool ok, const std::string& why) {
    session_.reportEdit(ok, why);
    refresh();
    refreshPanel();
    return ok;
}

void InsertModel::onCatalog(const api::PluginCatalog& c) {
    ctl_.set_catalog(c);
    refresh();
    refreshPanel();
    emit browserChanged();
}

void InsertModel::onFault(uint64_t insert, const std::string& text) {
    if (!ctl_.set_fault(insert, text)) return;
    refresh();
    refreshPanel();
}

void InsertModel::onFrame(const EngineFrame& f, bool alive) {
    if (!ctl_.set_frame(f, alive)) return;
    refresh();
    refreshPanel();
}

void InsertModel::onLinkReplaced() {
    ctl_.clear_faults();
    refresh();
    refreshPanel();
}

void InsertModel::refresh() {
    QVariantList strips;
    const int n = ctl_.track_count();
    for (int t = 0; t < n; ++t) {
        QVariantList cells;
        for (int s = 0; s < static_cast<int>(kInsertSlots); ++s) cells << slot_map(ctl_.slot(t, s), s);
        QVariantMap strip;
        const int64_t latency = ctl_.track_latency(t);
        strip.insert(QStringLiteral("slots"), cells);
        strip.insert(QStringLiteral("latency"), static_cast<qreal>(latency));
        strip.insert(QStringLiteral("latencyText"), latencyText(static_cast<qreal>(latency)));
        strips << strip;
    }
    // Re-published only when something shown changed: the state block
    // arrives at 15 Hz and is nearly always the same.
    const QVariantMap summary{{QStringLiteral("compensation"), compensationText()},
                              {QStringLiteral("short"), compensationShort()},
                              {QStringLiteral("disabled"), pluginsDisabled()},
                              {QStringLiteral("catalog"), hasCatalog()},
                              {QStringLiteral("categories"), categories()}};
    const bool same = strips == strips_ && summary == summary_;
    strips_ = std::move(strips);
    summary_ = summary;
    if (!same) emit changed();
}

QStringList InsertModel::categories() const {
    QStringList out;
    for (const std::string& c : ctl_.categories()) out << q(c);
    return out;
}

QString InsertModel::latencyText(qreal frames) const {
    if (frames < 0) return QStringLiteral("dly –");
    return QStringLiteral("dly %1").arg(static_cast<qint64>(frames));
}

QString InsertModel::compensationText() const {
    const int64_t c = ctl_.compensation();
    if (c < 0) return QStringLiteral("Delay compensation: no engine data");
    const uint32_t rate = ctl_.snapshot().settings.sample_rate;
    const double ms = rate > 0 ? static_cast<double>(c) * 1000.0 / rate : 0.0;
    QString text = QStringLiteral("Delay compensation %1 samples (%2 ms)")
                       .arg(QLocale::c().toString(static_cast<qlonglong>(c)))
                       .arg(ms, 0, 'f', 1);
    const int64_t s = ctl_.compensation_shortfall();
    if (s > 0) {
        text += QStringLiteral(", %1 samples short: the inserts need more than the maximum")
                    .arg(QLocale::c().toString(static_cast<qlonglong>(s)));
    }
    return text;
}

// --- slots ---

void InsertModel::slotClicked(int track, int slot, int modifiers) {
    const SlotView v = ctl_.slot(track, slot);
    if (!v.present) {
        openBrowser(track, slot);
        return;
    }
    // Pro Tools' gestures under every key-map profile: the key-map format
    // has no mouse section yet (BACKLOG, insert-slot mouse gestures).
    const bool ctrl = (modifiers & Qt::ControlModifier) != 0;
    const bool start = (modifiers & Qt::MetaModifier) != 0;
    std::string why;
    if (ctrl && start) {
        report(ctl_.set_active(track, slot, !v.active, why), why);
    } else if (ctrl) {
        report(ctl_.toggle_bypass(track, slot, why), why);
    } else {
        openPanel(track, slot);
    }
}

void InsertModel::dropInsert(int ft, int fs, int tt, int ts, bool copy) {
    std::string why;
    if (!copy) {
        report(ctl_.move(ft, fs, tt, ts, why), why);
        return;
    }
    ctl_.copy(ft, fs, tt, ts, [this](bool ok, const std::string& text) {
        report(ok, ok ? std::string() : text);
        if (ok && !text.empty()) emit session_.notice(q(text));
    });
}

void InsertModel::toggleBypass(int track, int slot) {
    std::string why;
    report(ctl_.toggle_bypass(track, slot, why), why);
}

void InsertModel::setActive(int track, int slot, bool active) {
    std::string why;
    report(ctl_.set_active(track, slot, active, why), why);
}

void InsertModel::remove(int track, int slot) {
    std::string why;
    report(ctl_.remove(track, slot, why), why);
}

// --- the chooser ---

QVariantList InsertModel::browse(const QString& category, const QString& search) const {
    QVariantList out;
    for (const int i : ctl_.browse(category.toStdString(), search.toStdString())) {
        const api::PluginInfo& p = ctl_.plugins()[static_cast<std::size_t>(i)];
        QVariantMap m;
        m.insert(QStringLiteral("index"), i);
        m.insert(QStringLiteral("name"), q(p.name));
        m.insert(QStringLiteral("vendor"), q(p.vendor));
        m.insert(QStringLiteral("format"), QString::fromLatin1(format_name(p.format)));
        m.insert(QStringLiteral("channels"), channels_name(p.inputs));
        QStringList cats;
        for (const std::string& c : p.categories) {
            if (c != "Fx") cats << q(c);
        }
        m.insert(QStringLiteral("category"), cats.join(QStringLiteral(" / ")));
        m.insert(QStringLiteral("refusal"), q(ctl_.refusal(i, browser_track_)));
        out << m;
    }
    return out;
}

QString InsertModel::browserTitle() const {
    if (browser_track_ < 0) return {};
    const auto& ts = ctl_.snapshot().edit->tracks;
    const QString track = browser_track_ < static_cast<int>(ts.size())
                              ? q(ts[static_cast<std::size_t>(browser_track_)]->name)
                              : QString();
    return QStringLiteral("%1 · Insert %2%3")
        .arg(track, q(slot_letter(browser_slot_)),
             browser_replaces_ ? QStringLiteral(" · replace") : QString());
}

void InsertModel::openBrowser(int track, int slot) {
    if (track < 0 || track >= ctl_.track_count() || slot < 0 ||
        slot >= static_cast<int>(kInsertSlots)) {
        return;
    }
    browser_track_ = track;
    browser_slot_ = slot;
    browser_replaces_ = ctl_.slot(track, slot).present;
    emit browserChanged();
}

void InsertModel::closeBrowser() {
    if (browser_track_ < 0) return;
    browser_track_ = browser_slot_ = -1;
    browser_replaces_ = false;
    emit browserChanged();
}

bool InsertModel::choose(int index) {
    if (browser_track_ < 0) return false;
    std::string why;
    const bool ok = browser_replaces_ ? ctl_.replace(browser_track_, browser_slot_, index, why)
                                      : ctl_.add(browser_track_, browser_slot_, index, why);
    report(ok, why);
    if (ok) closeBrowser();
    return ok;
}

// --- the parameter panel ---

void InsertModel::openPanel(int track, int slot) {
    const SlotView v = ctl_.slot(track, slot);
    if (!v.present) return;
    panel_insert_ = v.insert;
    refreshPanel();
    emit panelChanged();
}

void InsertModel::closePanel() {
    if (panel_insert_ == kNoId) return;
    std::string why;
    if (ctl_.gesture_running()) report(ctl_.param_end(why), why);
    panel_insert_ = kNoId;
    panel_track_ = panel_slot_ = -1;
    emit panelChanged();
}

void InsertModel::locatePanel() {
    if (panel_insert_ == kNoId) return;
    if (!ctl_.locate(panel_insert_, panel_track_, panel_slot_)) {
        panel_insert_ = kNoId;
        panel_track_ = panel_slot_ = -1;
    }
}

void InsertModel::refreshPanel() {
    const bool was_open = panel_insert_ != kNoId;
    locatePanel();
    if (panel_insert_ == kNoId) {
        if (was_open) emit panelChanged();
        return;
    }
    const SlotView v = ctl_.slot(panel_track_, panel_slot_);
    const auto& t = *ctl_.snapshot().edit->tracks[static_cast<std::size_t>(panel_track_)];
    const api::PluginInfo* p = ctl_.plugin_of(panel_insert_);

    QString title = q(v.name);
    QString subtitle = QStringLiteral("%1 · Insert %2").arg(q(t.name), q(slot_letter(panel_slot_)));
    if (p != nullptr) {
        subtitle += QStringLiteral(" · %1 %2 · %3")
                        .arg(QString::fromLatin1(format_name(p->format)), q(p->version), q(p->vendor));
    }
    if (v.latency_frames > 0) subtitle += QStringLiteral(" · %1 samples").arg(v.latency_frames);

    QVariantList params;
    QVariantMap values;
    QString note;
    int past_cap = 0;
    if (p == nullptr) {
        note = q(v.name) + QStringLiteral(" is not in this engine's catalog, so its parameters are "
                                          "unknown here. Its saved values are kept as they are.");
    } else {
        for (const api::PluginParam& each : p->params) {
            if ((each.flags & api::kParamPastCap) != 0) {
                ++past_cap;
                continue;
            }
            const float value = ctl_.value(panel_insert_, each);
            const int step = param_step(each, value);
            int steps = 0;
            QStringList labels;
            if (step >= 0) {
                steps = static_cast<int>(each.steps >= 2 ? each.steps : 2);
                for (int i = 0; i < steps; ++i) {
                    labels << q(param_text(*p, each, param_step_value(each, i)));
                }
            }
            QVariantMap m;
            m.insert(QStringLiteral("key"), q(each.key));
            m.insert(QStringLiteral("name"), q(each.name));
            m.insert(QStringLiteral("steps"), steps);
            m.insert(QStringLiteral("labels"), labels);
            params << m;

            QVariantMap now;
            now.insert(QStringLiteral("text"), q(param_text(*p, each, value)));
            now.insert(QStringLiteral("position"), param_position(each, value));
            now.insert(QStringLiteral("step"), step);
            values.insert(q(each.key), now);
        }
        if (params.isEmpty() && past_cap == 0) note = QStringLiteral("This plugin has no parameters.");
        if (past_cap > 0) {
            note = QStringLiteral("%1 more parameters are past what one chain can automate; they "
                                  "are kept in the plugin's own state.")
                       .arg(past_cap);
        }
    }
    const QVariantMap info = slot_map(v, panel_slot_);
    if (values != panel_values_) {
        panel_values_ = values;
        emit panelValuesChanged();
    }
    if (title == panel_title_ && subtitle == panel_subtitle_ && params == panel_params_ &&
        note == panel_note_ && info == panel_slot_info_ && was_open) {
        return;
    }
    panel_title_ = title;
    panel_subtitle_ = subtitle;
    panel_params_ = params;
    panel_note_ = note;
    panel_slot_info_ = info;
    emit panelChanged();
}

void InsertModel::paramBegin() {
    if (panel_insert_ == kNoId) return;
    std::string why;
    if (!ctl_.param_begin(panel_insert_, why)) report(false, why);
}

void InsertModel::paramPosition(const QString& key, qreal position) {
    if (panel_insert_ == kNoId) return;
    const api::PluginInfo* p = ctl_.plugin_of(panel_insert_);
    if (p == nullptr) return;
    for (const api::PluginParam& each : p->params) {
        if (q(each.key) != key) continue;
        std::string why;
        if (!ctl_.param_set(panel_insert_, each.key, param_value_at(each, static_cast<float>(position)),
                            why)) {
            report(false, why);
        }
        refreshPanel();
        return;
    }
}

void InsertModel::paramEnd() {
    std::string why;
    report(ctl_.param_end(why), why);
}

void InsertModel::paramStep(const QString& key, int step) {
    if (panel_insert_ == kNoId) return;
    const api::PluginInfo* p = ctl_.plugin_of(panel_insert_);
    if (p == nullptr) return;
    for (const api::PluginParam& each : p->params) {
        if (q(each.key) != key) continue;
        std::string why;
        // A choice is a gesture of one: the change, then its record.
        const bool ok = ctl_.param_set(panel_insert_, each.key, param_step_value(each, step), why) &&
                        ctl_.param_end(why);
        report(ok, why);
        return;
    }
}

void InsertModel::paramReset(const QString& key) {
    if (panel_insert_ == kNoId) return;
    const api::PluginInfo* p = ctl_.plugin_of(panel_insert_);
    if (p == nullptr) return;
    for (const api::PluginParam& each : p->params) {
        if (q(each.key) != key) continue;
        std::string why;
        const bool ok = ctl_.param_set(panel_insert_, each.key, each.default_value, why) &&
                        ctl_.param_end(why);
        report(ok, why);
        return;
    }
}

}  // namespace sonora::ui
