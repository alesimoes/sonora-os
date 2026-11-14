// SPDX-License-Identifier: GPL-3.0-or-later
//
// Show/Hide Tracks and track heights (ADR-0106): the Tracks list window and
// the four interface commands (track.height, track.hide, track.show_all,
// window.tracks_list). Answers the Edit view's row geometry as its RowPolicy.
//
// Interface state, kept per machine by the store and keyed by session id, so
// a session opened later starts from its own views and never from the last
// one's. An armed track is never hidden: its meter and its record state are
// the only sign to the operator that a take will happen, and a row that is
// not on screen cannot show them. The refusal is held until dismissed.
#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <cstdint>
#include <functional>
#include <string>

#include "sonora/command.h"
#include "track_row_geometry.h"
#include "track_views.h"

namespace sonora::ui {

// What the model reads of the session and the engine, injected so a test
// needs neither.
struct TrackViewSource {
    std::function<std::string()> session_id;
    std::function<int()> count;
    std::function<std::uint64_t(int)> track_id;
    std::function<std::string(int)> name;
    std::function<bool(int)> armed;
};

class TrackViewModel : public QObject, public RowPolicy {
    Q_OBJECT
    Q_PROPERTY(bool open READ open WRITE setOpen NOTIFY openChanged)
    // One entry per track, in display order: {index, name, height, hidden, armed}.
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
    Q_PROPERTY(int hiddenCount READ hiddenCount NOTIFY changed)
    // The last refusal, held until dismissed or the session changes.
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)

public:
    TrackViewModel(TrackViewStore& store, TrackViewSource source, QObject* parent = nullptr);

    void installCommands(CommandRegistry& local);

    // RowPolicy.
    bool active() const override;
    RowSpec row(int index, std::uint64_t track) const override;

    bool open() const { return open_; }
    void setOpen(bool on);
    QVariantList rows() const;
    int hiddenCount() const;
    QString notice() const;

    // False, with the reason in `why`, when nothing changed.
    bool setHeight(int index, const std::string& preset, std::string& why);
    bool hide(int index, std::string& why);
    bool show(int index);
    bool showAll(std::string& why);

    // For the window: the same, with a refusal held in `notice`.
    Q_INVOKABLE bool setHeightHeld(int index, const QString& preset);
    Q_INVOKABLE bool setHiddenHeld(int index, bool hidden);
    Q_INVOKABLE bool showAllHeld();
    Q_INVOKABLE void dismissNotice();

signals:
    void openChanged();
    void changed();
    void noticeChanged();
    // A refusal the operator must see: the shell shows it until dismissed.
    void heldNotice(const QString& text);

private:
    // Drops what belonged to the session that was open before.
    void checkSession() const;
    bool valid(int index, std::string& why) const;
    bool armed(int index) const;
    TrackView view(int index) const;
    void hold(const std::string& why);
    void touched();
    void write(int index, const TrackView& v);

    TrackViewStore& store_;
    TrackViewSource src_;
    bool open_ = false;
    mutable QString notice_;
    mutable std::string seen_session_;
    // active() runs on every row layout; the scan behind it is redone only
    // when the session, the track count or a view changed.
    mutable std::string cache_session_;
    mutable int cache_count_ = -1;
    mutable std::uint64_t generation_ = 0, cache_generation_ = ~std::uint64_t{0};
    mutable bool cache_active_ = false;
};

}  // namespace sonora::ui
