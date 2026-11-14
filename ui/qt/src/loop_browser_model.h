// SPDX-License-Identifier: GPL-3.0-or-later
//
// The loop browser (ADR-0163): the catalog behind a filter bar, with audition
// and insert. Everything reaches the world through ILoopSource,
// ILoopAudition and ILoopInserter, so nothing here touches the engine or
// the audio thread. A refusal (cannot play, track unknown, nothing to
// insert onto) is a held notice, never the 2.5 s status line, and an
// audition that did not start never shows as playing.
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "sonora/loops/loop_source.h"

namespace sonora::ui {

class LoopBrowserModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
    Q_PROPERTY(QStringList categories READ categories NOTIFY changed)
    Q_PROPERTY(QStringList keys READ keys NOTIFY changed)
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY changed)
    Q_PROPERTY(QString category READ category WRITE setCategory NOTIFY changed)
    Q_PROPERTY(QString key READ key WRITE setKey NOTIFY changed)
    // Match the session tempo within `tolerance` percent when on.
    Q_PROPERTY(bool matchTempo READ matchTempo WRITE setMatchTempo NOTIFY changed)
    Q_PROPERTY(double sessionBpm READ sessionBpm NOTIFY changed)
    Q_PROPERTY(int tolerance READ tolerance WRITE setTolerance NOTIFY changed)
    Q_PROPERTY(QString selectedId READ selectedId NOTIFY changed)
    Q_PROPERTY(QString auditioningId READ auditioningId NOTIFY changed)
    Q_PROPERTY(QString summary READ summary NOTIFY changed)
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)

public:
    // What the browser needs of the session, injected.
    struct Context {
        std::function<double()> session_bpm;  // 0 = no tempo
        std::function<int()> target_track;    // -1 = none selected
        std::function<std::uint64_t()> insert_position;
    };

    LoopBrowserModel(loops::ILoopSource& source, loops::ILoopAudition& audition,
                     loops::ILoopInserter& inserter, Context context,
                     QObject* parent = nullptr);

    // [{id, name, category, key, bpm, beats, tags, selected, auditioning}]
    QVariantList rows() const;
    QStringList categories() const;
    QStringList keys() const;
    QString text() const { return text_; }
    QString category() const { return category_; }
    QString key() const { return key_; }
    bool matchTempo() const { return match_tempo_; }
    double sessionBpm() const;
    int tolerance() const { return tolerance_; }
    QString selectedId() const { return selected_; }
    QString auditioningId() const;
    QString summary() const;
    QString notice() const { return notice_; }

    void setText(const QString& t);
    void setCategory(const QString& c);
    void setKey(const QString& k);
    void setMatchTempo(bool on);
    void setTolerance(int percent);

    const std::vector<loops::LoopEntry>& filtered() const { return filtered_; }

    Q_INVOKABLE void select(const QString& id);
    // Toggles: a second press on the playing entry stops it.
    Q_INVOKABLE bool audition(const QString& id);
    Q_INVOKABLE void stopAudition();
    Q_INVOKABLE bool insertSelected();
    Q_INVOKABLE bool insertEntry(const QString& id);
    Q_INVOKABLE void dismissNotice();

signals:
    void changed();
    void noticeChanged();

private:
    void refilter();
    const loops::LoopEntry* find(const QString& id) const;
    bool refuse(const QString& why);

    loops::ILoopSource& source_;
    loops::ILoopAudition& audition_;
    loops::ILoopInserter& inserter_;
    Context context_;
    std::vector<loops::LoopEntry> filtered_;
    QString text_, category_, key_, selected_, playing_, notice_;
    bool match_tempo_ = false;
    int tolerance_ = 5;
};

}  // namespace sonora::ui
