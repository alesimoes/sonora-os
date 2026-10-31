// SPDX-License-Identifier: GPL-3.0-or-later
#include "send_model.h"

#include "fader_taper.h"
#include "session_model.h"

namespace sonora::ui {

using namespace model;

namespace {

QString q(const std::string& s) { return QString::fromStdString(s); }

QString letter(int slot) { return QString(QChar('A' + slot)); }

// The strip a model id names now, tracks then buses; -1 when it is gone.
int strip_of(const Snapshot& s, Id id) {
    const auto& tracks = s.edit->tracks;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (tracks[i]->id == id) return static_cast<int>(i);
    }
    const auto& buses = s.edit->buses;
    for (std::size_t b = 0; b < buses.size(); ++b) {
        if (buses[b].id == id) return static_cast<int>(tracks.size() + b);
    }
    return -1;
}

Id id_of(const Snapshot& s, int strip) {
    const auto& tracks = s.edit->tracks;
    if (strip >= 0 && strip < static_cast<int>(tracks.size())) {
        return tracks[static_cast<std::size_t>(strip)]->id;
    }
    const int b = strip - static_cast<int>(tracks.size());
    if (b >= 0 && b < static_cast<int>(s.edit->buses.size())) {
        return s.edit->buses[static_cast<std::size_t>(b)].id;
    }
    return kNoId;
}

}  // namespace

SendModel::SendModel(TimelineDocument& doc, SessionModel& session, QObject* parent)
    : QObject(parent), ctl_(doc), session_(session) {
    connect(&session_, &SessionModel::mixChanged, this, [this]() {
        refresh();
        refreshWindow();
    });
    connect(&session_, &SessionModel::contentChanged, this, [this]() {
        refresh();
        refreshWindow();
    });
    // Which five sends a strip shows is the window's view of one session.
    connect(&session_, &SessionModel::sessionOpened, this, [this]() {
        dropRide();   // another session: the ride's ids mean nothing in it
        setBank(0);
    });
    refresh();
}

bool SendModel::reportRouting(bool ok, const std::string& why) {
    session_.reportRouting(ok, why);
    refresh();
    refreshWindow();
    return ok;
}

bool SendModel::report(bool ok, const std::string& why) {
    session_.reportEdit(ok, why);
    refresh();
    refreshWindow();
    return ok;
}

void SendModel::refresh() {
    strips_.clear();
    for (int s = 0; s < ctl_.strip_count(); ++s) {
        QVariantMap strip;
        strip.insert(QStringLiteral("name"), q(ctl_.strip_name(s)));
        strip.insert(QStringLiteral("sends"), ctl_.strip_sends(s));
        strip.insert(QStringLiteral("output"), q(ctl_.output_name(s)));
        QVariantList cells;
        for (int k = 0; k < kSendSlots; ++k) {
            const SendSlotView v = ctl_.slot(s, k);
            QVariantMap m;
            m.insert(QStringLiteral("present"), v.present);
            m.insert(QStringLiteral("letter"), letter(k));
            m.insert(QStringLiteral("bus"), q(v.bus_name));
            m.insert(QStringLiteral("levelText"), q(gain_text(v.level_db)));
            m.insert(QStringLiteral("pre"), v.pre);
            m.insert(QStringLiteral("mute"), v.mute);
            cells.append(m);
        }
        strip.insert(QStringLiteral("slots"), cells);
        strips_.append(strip);
    }
    auxes_.clear();
    for (int b = 0; b < ctl_.bus_count(); ++b) {
        const AuxView v = ctl_.aux(b);
        QVariantMap m;
        m.insert(QStringLiteral("name"), q(v.name));
        m.insert(QStringLiteral("gainDb"), v.gain_db);
        m.insert(QStringLiteral("gainText"), q(gain_text(v.gain_db)));
        m.insert(QStringLiteral("pan"), v.pan);
        m.insert(QStringLiteral("panText"), q(pan_text(v.pan)));
        m.insert(QStringLiteral("mute"), v.mute);
        m.insert(QStringLiteral("solo"), v.solo);
        m.insert(QStringLiteral("output"), q(ctl_.output_name(ctl_.track_count() + b)));
        auxes_.append(m);
    }
    overlayRide();
    // A chooser over a strip that has gone is closed, not left on another.
    if (chooser_strip_ >= ctl_.strip_count()) {
        chooser_strip_ = chooser_slot_ = -1;
        emit chooserChanged();
    }
    emit changed();
}

void SendModel::refreshWindow() {
    if (window_strip_id_ == kNoId) return;
    const int strip = strip_of(ctl_.snapshot(), window_strip_id_);
    const SendSlotView v = strip >= 0 ? ctl_.slot(strip, window_slot_) : SendSlotView{};
    if (!v.present) {
        // Removed here or elsewhere, or undone: the window has nothing left.
        closeWindow();
        return;
    }
    window_strip_ = strip;
    QVariantMap w;
    w.insert(QStringLiteral("title"),
             q(ctl_.strip_name(strip)) + QStringLiteral(": send ") + letter(window_slot_));
    w.insert(QStringLiteral("bus"), q(v.bus_name));
    w.insert(QStringLiteral("levelDb"), v.level_db);
    w.insert(QStringLiteral("levelText"), q(gain_text(v.level_db)));
    w.insert(QStringLiteral("pan"), v.pan);
    w.insert(QStringLiteral("panText"), q(pan_text(v.pan)));
    w.insert(QStringLiteral("followPan"), v.follow_pan);
    w.insert(QStringLiteral("pre"), v.pre);
    w.insert(QStringLiteral("mute"), v.mute);
    window_ = w;
    overlayRide();
    emit windowChanged();
}

void SendModel::setBank(int first) {
    const int b = first >= 5 ? 5 : 0;
    if (b == bank_) return;
    bank_ = b;
    emit bankChanged();
}

void SendModel::slotClicked(int strip, int slot) {
    if (!ctl_.strip_sends(strip)) {
        std::string why = "\"" + ctl_.strip_name(strip) + "\" is a MIDI track; it has no audio to send";
        report(false, why);
        return;
    }
    if (ctl_.slot(strip, slot).present) {
        openWindow(strip, slot);
    } else {
        openChooser(strip, slot);
    }
}

QString SendModel::chooserTitle() const {
    if (chooser_strip_ < 0) return {};
    if (chooser_output_) return q(ctl_.strip_name(chooser_strip_)) + QStringLiteral(": output to");
    return q(ctl_.strip_name(chooser_strip_)) + QStringLiteral(": send ") + letter(chooser_slot_) +
           QStringLiteral(" to");
}

QVariantList SendModel::chooserItems() const {
    QVariantList out;
    if (chooser_strip_ < 0) return out;
    const std::vector<SendDestination> items = chooser_output_
        ? ctl_.output_choices(chooser_strip_)
        : ctl_.destinations(chooser_strip_, chooser_slot_);
    for (const SendDestination& d : items) {
        QVariantMap m;
        m.insert(QStringLiteral("bus"), d.bus);
        m.insert(QStringLiteral("name"), q(d.name));
        m.insert(QStringLiteral("refusal"), q(d.refusal));
        out.append(m);
    }
    return out;
}

void SendModel::openChooser(int strip, int slot) {
    if (strip < 0 || strip >= ctl_.strip_count() || slot < 0 || slot >= kSendSlots) return;
    chooser_strip_ = strip;
    chooser_slot_ = slot;
    chooser_output_ = false;
    emit chooserChanged();
}

void SendModel::openOutputChooser(int strip) {
    if (strip < 0 || strip >= ctl_.strip_count()) return;
    if (!ctl_.strip_sends(strip)) {
        reportRouting(false, "\"" + ctl_.strip_name(strip) + "\" is a MIDI track; it has no audio output");
        return;
    }
    chooser_strip_ = strip;
    chooser_slot_ = -1;
    chooser_output_ = true;
    emit chooserChanged();
}

void SendModel::closeChooser() {
    if (chooser_strip_ < 0) return;
    chooser_strip_ = chooser_slot_ = -1;
    chooser_output_ = false;
    emit chooserChanged();
}

bool SendModel::choose(int bus) {
    if (chooser_strip_ < 0) return false;
    const int strip = chooser_strip_;
    const int slot = chooser_slot_;
    std::string why;
    if (chooser_output_) {
        const bool ok = ctl_.set_output(strip, bus < 0 ? SendController::kMaster : bus, why);
        if (ok) closeChooser();
        return reportRouting(ok, why);
    }
    const bool ok = bus < 0 ? ctl_.assign_new_bus(strip, slot, why)
                            : ctl_.assign(strip, slot, bus, why);
    if (ok) closeChooser();
    return report(ok, why);
}

void SendModel::openWindow(int strip, int slot) {
    if (!ctl_.slot(strip, slot).present) return;
    window_strip_id_ = id_of(ctl_.snapshot(), strip);
    window_strip_ = strip;
    window_slot_ = slot;
    refreshWindow();
}

void SendModel::closeWindow() {
    if (window_strip_id_ == kNoId) return;
    window_strip_id_ = kNoId;
    window_strip_ = window_slot_ = -1;
    window_.clear();
    emit windowChanged();
}

void SendModel::sendLive(float db) {
    sender_(ride_ == Ride::Aux, ride_id_, fader_linear_gain(db));
    ++live_steps_;
    ride_sent_ = true;
}

void SendModel::showRide() {
    overlayRide();
    if (ride_ == Ride::Send) emit windowChanged();
    else if (ride_ == Ride::Aux) emit changed();
}

void SendModel::overlayRide() {
    if (ride_ == Ride::Send) {
        if (window_strip_id_ == ride_strip_id_ && window_slot_ == ride_slot_) {
            window_.insert(QStringLiteral("levelDb"), ride_db_);
            window_.insert(QStringLiteral("levelText"), q(gain_text(ride_db_)));
        }
    } else if (ride_ == Ride::Aux) {
        const int b = strip_of(ctl_.snapshot(), ride_id_) - ctl_.track_count();
        if (b >= 0 && b < auxes_.size()) {
            QVariantMap m = auxes_[b].toMap();
            m.insert(QStringLiteral("gainDb"), ride_db_);
            m.insert(QStringLiteral("gainText"), q(gain_text(ride_db_)));
            auxes_[b] = m;
        }
    }
}

void SendModel::dropRide() {
    ride_ = Ride::None;
    ride_id_ = ride_strip_id_ = kNoId;
    ride_slot_ = -1;
    ride_sent_ = false;
}

void SendModel::beginLevelRide() {
    if (!windowOpen() || !sender_) return;
    const SendSlotView v = ctl_.slot(window_strip_, window_slot_);
    if (!v.present) return;
    ride_ = Ride::Send;
    ride_id_ = v.send;
    ride_strip_id_ = window_strip_id_;
    ride_slot_ = window_slot_;
    ride_db_ = v.level_db;
    ride_sent_ = false;
}

void SendModel::beginAuxRide(int bus) {
    if (!sender_ || bus < 0 || bus >= ctl_.bus_count()) return;
    ride_ = Ride::Aux;
    ride_id_ = ctl_.aux(bus).id;
    ride_db_ = ctl_.aux(bus).gain_db;
    ride_sent_ = false;
}

void SendModel::finishRide(bool save) {
    const Ride kind = ride_;
    const model::Id id = ride_id_;
    const model::Id strip_id = ride_strip_id_;
    const int slot = ride_slot_;
    const float db = ride_db_;
    const bool sent = ride_sent_;
    dropRide();
    if (!sent) return;
    std::string why;
    bool ok = false;
    if (save && kind == Ride::Send) {
        const int strip = strip_of(ctl_.snapshot(), strip_id);
        ok = strip >= 0 && ctl_.slot(strip, slot).send == id &&
             ctl_.set_level(strip, slot, db, why);
        if (strip < 0 || ctl_.slot(strip, slot).send != id) why = "the send being ridden is gone";
    } else if (save) {
        const int b = strip_of(ctl_.snapshot(), id) - ctl_.track_count();
        ok = b >= 0 && ctl_.set_bus_gain(b, db, why);
        if (b < 0) why = "the aux bus being ridden is gone";
    }
    if (save) session_.reportEdit(ok, why);
    // The engine is where the ride left it; if the document refused that, or
    // the ride was dropped, it goes back to what the document holds.
    if (!ok && sender_) {
        if (kind == Ride::Send) {
            const int strip = strip_of(ctl_.snapshot(), strip_id);
            if (strip >= 0 && ctl_.slot(strip, slot).send == id) {
                sender_(false, id, fader_linear_gain(ctl_.slot(strip, slot).level_db));
            }
        } else {
            const int b = strip_of(ctl_.snapshot(), id) - ctl_.track_count();
            if (b >= 0) sender_(true, id, fader_linear_gain(ctl_.aux(b).gain_db));
        }
    }
    refresh();
    refreshWindow();
}

void SendModel::endLevelRide() {
    if (ride_ == Ride::Send) finishRide(true);
}

void SendModel::endAuxRide(int bus) {
    if (ride_ == Ride::Aux && bus >= 0 && bus < ctl_.bus_count() && ctl_.aux(bus).id == ride_id_) {
        finishRide(true);
    }
}

void SendModel::levelDrag(qreal dy, qreal travel, bool fine) {
    if (!windowOpen()) return;
    if (ride_ == Ride::Send) {
        ride_db_ = fader_drag(ride_db_, static_cast<float>(dy), static_cast<float>(travel), fine);
        sendLive(ride_db_);
        showRide();
        return;
    }
    const float from = ctl_.slot(window_strip_, window_slot_).level_db;
    const float db = fader_drag(from, static_cast<float>(dy), static_cast<float>(travel), fine);
    std::string why;
    report(ctl_.set_level(window_strip_, window_slot_, db, why), why);
}

bool SendModel::setLevelText(const QString& text) {
    if (!windowOpen()) return false;
    float db = 0.0f;
    std::string why;
    if (!parse_gain_text(text.toStdString(), db, why)) return report(false, why);
    return report(ctl_.set_level(window_strip_, window_slot_, db, why), why);
}

void SendModel::resetLevel() {
    if (!windowOpen()) return;
    std::string why;
    report(ctl_.set_level(window_strip_, window_slot_, 0.0f, why), why);
}

void SendModel::panDrag(qreal dx, qreal width, bool fine) {
    if (!windowOpen() || width <= 0) return;
    const float from = ctl_.slot(window_strip_, window_slot_).pan;
    const float step = static_cast<float>(2.0 * dx / width) * (fine ? 0.1f : 1.0f);
    std::string why;
    report(ctl_.set_pan(window_strip_, window_slot_, from + step, why), why);
}

bool SendModel::setPanText(const QString& text) {
    if (!windowOpen()) return false;
    float pan = 0.0f;
    std::string why;
    if (!parse_pan_text(text.toStdString(), pan, why)) return report(false, why);
    return report(ctl_.set_pan(window_strip_, window_slot_, pan, why), why);
}

void SendModel::togglePre() {
    if (!windowOpen()) return;
    std::string why;
    report(ctl_.toggle_pre(window_strip_, window_slot_, why), why);
}

void SendModel::toggleMute() {
    if (!windowOpen()) return;
    std::string why;
    report(ctl_.toggle_mute(window_strip_, window_slot_, why), why);
}

void SendModel::toggleFollowPan() {
    if (!windowOpen()) return;
    std::string why;
    report(ctl_.toggle_follow_pan(window_strip_, window_slot_, why), why);
}

void SendModel::removeSend() {
    if (!windowOpen()) return;
    std::string why;
    report(ctl_.remove(window_strip_, window_slot_, why), why);
}

void SendModel::reassign() {
    if (!windowOpen()) return;
    openChooser(window_strip_, window_slot_);
}

void SendModel::newBus() {
    std::string why;
    report(ctl_.new_bus(why), why);
}

void SendModel::removeBus(int bus) {
    std::string why;
    report(ctl_.remove_bus(bus, why), why);
}

void SendModel::auxFaderDrag(int bus, qreal dy, qreal travel, bool fine) {
    if (ride_ == Ride::Aux && bus >= 0 && bus < ctl_.bus_count() && ctl_.aux(bus).id == ride_id_) {
        ride_db_ = fader_drag(ride_db_, static_cast<float>(dy), static_cast<float>(travel), fine);
        sendLive(ride_db_);
        showRide();
        return;
    }
    const float from = ctl_.aux(bus).gain_db;
    const float db = fader_drag(from, static_cast<float>(dy), static_cast<float>(travel), fine);
    std::string why;
    report(ctl_.set_bus_gain(bus, db, why), why);
}

bool SendModel::setAuxGainText(int bus, const QString& text) {
    float db = 0.0f;
    std::string why;
    if (!parse_gain_text(text.toStdString(), db, why)) return report(false, why);
    return report(ctl_.set_bus_gain(bus, db, why), why);
}

void SendModel::resetAuxGain(int bus) {
    std::string why;
    report(ctl_.set_bus_gain(bus, 0.0f, why), why);
}

void SendModel::auxPanDrag(int bus, qreal dx, qreal width, bool fine) {
    if (width <= 0) return;
    const float from = ctl_.aux(bus).pan;
    const float step = static_cast<float>(2.0 * dx / width) * (fine ? 0.1f : 1.0f);
    std::string why;
    report(ctl_.set_bus_pan(bus, from + step, why), why);
}

void SendModel::toggleAuxMute(int bus) {
    std::string why;
    report(ctl_.toggle_bus_mute(bus, why), why);
}

void SendModel::toggleAuxSolo(int bus) {
    std::string why;
    report(ctl_.toggle_bus_solo(bus, why), why);
}

}  // namespace sonora::ui
