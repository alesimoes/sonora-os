// SPDX-License-Identifier: GPL-3.0-or-later
#include "loop_browser_model.h"

#include <QVariantMap>
#include <algorithm>

namespace sonora::ui {

namespace {

QString q(const std::string& s) { return QString::fromStdString(s); }

QStringList distinct(const loops::ILoopSource& src, std::string loops::LoopEntry::*field) {
    QStringList out;
    for (std::size_t i = 0; i < src.count(); ++i) {
        const loops::LoopEntry* e = src.at(i);
        if (e == nullptr || (e->*field).empty()) continue;
        const QString v = q(e->*field);
        if (!out.contains(v)) out.push_back(v);
    }
    out.sort();
    return out;
}

}  // namespace

LoopBrowserModel::LoopBrowserModel(loops::ILoopSource& source, loops::ILoopAudition& audition,
                                   loops::ILoopInserter& inserter, Context context, QObject* parent)
    : QObject(parent), source_(source), audition_(audition), inserter_(inserter),
      context_(std::move(context)) {
    refilter();
}

double LoopBrowserModel::sessionBpm() const {
    return context_.session_bpm ? context_.session_bpm() : 0.0;
}

QString LoopBrowserModel::auditioningId() const {
    // The audition may end by itself; ask it, never remember.
    return audition_.playing() ? playing_ : QString();
}

QStringList LoopBrowserModel::categories() const {
    return distinct(source_, &loops::LoopEntry::category);
}
QStringList LoopBrowserModel::keys() const { return distinct(source_, &loops::LoopEntry::key); }

void LoopBrowserModel::refilter() {
    loops::LoopQuery query;
    query.text = text_.toStdString();
    query.category = category_.toStdString();
    query.key = key_.toStdString();
    if (match_tempo_ && sessionBpm() > 0.0) {
        query.bpm = sessionBpm();
        query.bpm_tolerance = tolerance_ / 100.0;
    }
    filtered_ = source_.search(query);
    std::sort(filtered_.begin(), filtered_.end(),
              [](const loops::LoopEntry& a, const loops::LoopEntry& b) { return a.name < b.name; });
    if (!find(selected_)) selected_.clear();
    emit changed();
}

QVariantList LoopBrowserModel::rows() const {
    QVariantList out;
    const QString playing = auditioningId();
    for (const loops::LoopEntry& e : filtered_) {
        QVariantMap m;
        QStringList tags;
        for (const std::string& t : e.tags) tags << q(t);
        m[QStringLiteral("id")] = q(e.id);
        m[QStringLiteral("name")] = q(e.name);
        m[QStringLiteral("category")] = q(e.category);
        m[QStringLiteral("key")] = q(e.key);
        m[QStringLiteral("bpm")] = e.bpm;
        m[QStringLiteral("beats")] = e.beats;
        m[QStringLiteral("tags")] = tags.join(QStringLiteral(", "));
        m[QStringLiteral("selected")] = q(e.id) == selected_;
        m[QStringLiteral("auditioning")] = q(e.id) == playing;
        out.push_back(m);
    }
    return out;
}

QString LoopBrowserModel::summary() const {
    return QStringLiteral("%1 of %2 loops").arg(filtered_.size()).arg(source_.count());
}

void LoopBrowserModel::setText(const QString& t) {
    if (t == text_) return;
    text_ = t;
    refilter();
}
void LoopBrowserModel::setCategory(const QString& c) {
    if (c == category_) return;
    category_ = c;
    refilter();
}
void LoopBrowserModel::setKey(const QString& k) {
    if (k == key_) return;
    key_ = k;
    refilter();
}
void LoopBrowserModel::setMatchTempo(bool on) {
    if (on == match_tempo_) return;
    match_tempo_ = on;
    refilter();
}
void LoopBrowserModel::setTolerance(int percent) {
    percent = std::clamp(percent, 0, 50);
    if (percent == tolerance_) return;
    tolerance_ = percent;
    refilter();
}

const loops::LoopEntry* LoopBrowserModel::find(const QString& id) const {
    if (id.isEmpty()) return nullptr;
    const std::string want = id.toStdString();
    for (const loops::LoopEntry& e : filtered_) {
        if (e.id == want) return &e;
    }
    return nullptr;
}

bool LoopBrowserModel::refuse(const QString& why) {
    notice_ = why;
    emit noticeChanged();
    return false;
}

void LoopBrowserModel::dismissNotice() {
    if (notice_.isEmpty()) return;
    notice_.clear();
    emit noticeChanged();
}

void LoopBrowserModel::select(const QString& id) {
    if (!find(id) || id == selected_) return;
    selected_ = id;
    emit changed();
}

bool LoopBrowserModel::audition(const QString& id) {
    const loops::LoopEntry* e = find(id);
    if (!e) return refuse(QStringLiteral("No such loop to audition."));
    if (audition_.playing() && playing_ == id) {
        stopAudition();
        return true;
    }
    select(id);
    if (!audition_.start(*e, sessionBpm())) {
        playing_.clear();
        refuse(QStringLiteral("Cannot play \"%1\": the file is missing or unreadable.").arg(q(e->name)));
        emit changed();
        return false;
    }
    playing_ = id;
    emit changed();
    return true;
}

void LoopBrowserModel::stopAudition() {
    audition_.stop();
    playing_.clear();
    emit changed();
}

bool LoopBrowserModel::insertSelected() { return insertEntry(selected_); }

bool LoopBrowserModel::insertEntry(const QString& id) {
    const loops::LoopEntry* e = find(id);
    if (!e) return refuse(QStringLiteral("Select a loop to insert."));
    const int track = context_.target_track ? context_.target_track() : -1;
    if (track < 0) return refuse(QStringLiteral("Select a track to insert \"%1\" onto.").arg(q(e->name)));
    const std::uint64_t at = context_.insert_position ? context_.insert_position() : 0;
    if (!inserter_.insert(*e, track, at))
        return refuse(QStringLiteral("\"%1\" was not inserted: the track or the file was refused.")
                          .arg(q(e->name)));
    return true;
}

}  // namespace sonora::ui
