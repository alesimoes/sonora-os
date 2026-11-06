// SPDX-License-Identifier: GPL-3.0-or-later
#include "io_setup_model.h"

#include <QStringList>
#include <QVariantMap>
#include <algorithm>

#include "session_model.h"
#include "sonora/model/operations.h"
#include "timeline_document.h"

namespace sonora::ui {

using namespace model;

namespace {

QString q(const std::string& s) { return QString::fromStdString(s); }

}  // namespace

IoSetupModel::IoSetupModel(TimelineDocument& doc, SessionModel& session, QObject* parent)
    : QObject(parent), doc_(doc), session_(session) {
    connect(&session_, &SessionModel::mixChanged, this, &IoSetupModel::refresh);
    connect(&session_, &SessionModel::contentChanged, this, &IoSetupModel::refresh);
    connect(&session_, &SessionModel::sessionChanged, this, &IoSetupModel::refresh);
    // A refusal is about a track of the session it was made in.
    connect(&session_, &SessionModel::sessionOpened, this, [this]() { setRefusal(QString()); });
    poll_.setInterval(500);
    connect(&poll_, &QTimer::timeout, this, &IoSetupModel::refresh);
    refresh();
}

void IoSetupModel::setEngine(Count inputs, Count outputs, Recording recording, Armed armed) {
    inputs_fn_ = std::move(inputs);
    outputs_fn_ = std::move(outputs);
    recording_fn_ = std::move(recording);
    armed_fn_ = std::move(armed);
    refresh();
}

void IoSetupModel::setNamesFile(const std::string& path) {
    names_file_ = path;
    names_ = load_io_names(path);
    refresh();
}

void IoSetupModel::setOpen(bool on) {
    if (on == open_) return;
    open_ = on;
    if (on) {
        refresh();
        poll_.start();
    } else {
        poll_.stop();
        setRefusal(QString());
    }
    emit openChanged();
}

void IoSetupModel::setRefusal(const QString& r) {
    if (r == refusal_) return;
    refusal_ = r;
    emit refusalChanged();
}

bool IoSetupModel::done(bool ok, const std::string& why) {
    setRefusal(ok ? QString() : q(why));
    return ok;
}

QString IoSetupModel::inputText(int first, int channels) const {
    QStringList parts;
    for (int c = 0; c < channels; ++c) {
        parts << q(names_.name(IoSide::Input, static_cast<std::size_t>(first + c)));
    }
    return parts.join(QStringLiteral(" + "));
}

void IoSetupModel::refresh() {
    input_count_ = inputs_fn_ ? inputs_fn_() : -1;
    output_count_ = outputs_fn_ ? std::max(0, outputs_fn_()) : 0;

    QVariantList ins;
    for (int i = 0; i < input_count_; ++i) {
        QVariantMap m;
        m[QStringLiteral("number")] = i + 1;
        m[QStringLiteral("name")] = q(names_.name(IoSide::Input, static_cast<std::size_t>(i)));
        m[QStringLiteral("renamed")] = names_.renamed(IoSide::Input, static_cast<std::size_t>(i));
        ins << m;
    }
    QVariantList outs;
    for (int i = 0; i < output_count_; ++i) {
        QVariantMap m;
        m[QStringLiteral("number")] = i + 1;
        m[QStringLiteral("name")] = q(names_.name(IoSide::Output, static_cast<std::size_t>(i)));
        m[QStringLiteral("renamed")] = names_.renamed(IoSide::Output, static_cast<std::size_t>(i));
        outs << m;
    }

    const Snapshot& s = doc_.snapshot();
    QVariantList rows;
    // edit->tracks is the display order, and engine track i is row i.
    for (std::size_t i = 0; i < s.edit->tracks.size(); ++i) {
        const TrackEdit& t = *s.edit->tracks[i];
        const TrackMix* mix = s.mix.find(t.id);
        QVariantMap m;
        m[QStringLiteral("row")] = static_cast<int>(i);
        m[QStringLiteral("name")] = q(t.name);
        const bool audio = t.kind == TrackKind::Audio;
        m[QStringLiteral("audio")] = audio;
        m[QStringLiteral("channels")] = static_cast<int>(t.channels);
        int first = -1;
        QString text;
        QString note;
        bool connected = false;
        if (!audio) {
            text = t.kind == TrackKind::Midi ? QStringLiteral("MIDI") : QStringLiteral("MIDI (instrument)");
        } else if (mix == nullptr || mix->input.empty()) {
            text = QStringLiteral("no input");
            note = QStringLiteral("no input: records nothing");
        } else {
            first = mix->input.front();
            text = inputText(first, static_cast<int>(mix->input.size()));
            const int last = mix->input.back();
            if (input_count_ < 0) {
                note = QStringLiteral("not known: the engine is not answering");
            } else if (last >= input_count_) {
                note = QStringLiteral("not connected: the interface has %1 input%2")
                           .arg(input_count_)
                           .arg(input_count_ == 1 ? QString() : QStringLiteral("s"));
            } else {
                connected = true;
            }
        }
        m[QStringLiteral("input")] = first;
        m[QStringLiteral("inputText")] = text;
        m[QStringLiteral("connected")] = connected;
        m[QStringLiteral("note")] = note;
        rows << m;
    }

    const bool same = ins == inputs_ && outs == outputs_ && rows == tracks_;
    inputs_ = std::move(ins);
    outputs_ = std::move(outs);
    tracks_ = std::move(rows);
    if (!same) emit changed();
}

bool IoSetupModel::rename(IoSide side, int index, const std::string& name, std::string& why) {
    refresh();
    const int count = side == IoSide::Input ? input_count_ : output_count_;
    if (count < 0) {
        why = "the engine is not answering, so which inputs this interface has is not known";
        return false;
    }
    if (index < 0) {
        why = "there is no such " + std::string(side == IoSide::Input ? "input" : "output");
        return false;
    }
    const auto at = static_cast<std::size_t>(index);
    // Trimmed, so " Kick In" and "Kick In" are one name.
    std::string n = name;
    while (!n.empty() && n.back() == ' ') n.pop_back();
    while (!n.empty() && n.front() == ' ') n.erase(n.begin());
    if (n.empty()) n = default_io_name(side, at);
    why = names_.problem(side, at, static_cast<std::size_t>(count), n);
    if (!why.empty()) return false;
    IoNames next = names_;
    next.set(side, at, n);
    if (next == names_) return true;
    if (!names_file_.empty() && !save_io_names(names_file_, next, why)) {
        why = "the name was not kept: " + why;
        return false;
    }
    names_ = std::move(next);
    refresh();
    // Every track row naming that input reads differently now.
    emit changed();
    return true;
}

bool IoSetupModel::renameInput(int index, const QString& name) {
    std::string why;
    return done(rename(IoSide::Input, index, name.toStdString(), why), why);
}

bool IoSetupModel::renameOutput(int index, const QString& name) {
    std::string why;
    return done(rename(IoSide::Output, index, name.toStdString(), why), why);
}

bool IoSetupModel::pickInput(int row, int input, std::string& why) {
    refresh();
    const Snapshot& s = doc_.snapshot();
    if (row < 0 || row >= static_cast<int>(s.edit->tracks.size())) {
        why = "there is no track " + std::to_string(row + 1);
        return false;
    }
    const auto index = static_cast<std::size_t>(row);
    const TrackEdit& t = *s.edit->tracks[index];
    if (t.kind != TrackKind::Audio) {
        why = t.name + (t.kind == TrackKind::Midi ? " is a MIDI track" : " is an instrument track") +
              ": it records MIDI, not an audio input";
        return false;
    }
    const TrackMix* mix = s.mix.find(t.id);
    if (mix == nullptr) {
        why = t.name + " has no mix settings";
        return false;
    }
    if (input_count_ < 0) {
        why = "the engine is not answering, so which inputs this interface has is not known; " +
              t.name + "'s input was not changed";
        return false;
    }
    const int channels = static_cast<int>(t.channels);
    if (input < 0 || input >= input_count_) {
        why = "there is no input " + std::to_string(input + 1) + "; the interface has " +
              std::to_string(input_count_);
        return false;
    }
    if (input + channels > input_count_) {
        why = t.name + " records " + std::to_string(channels) + " inputs side by side, and " +
              names_.name(IoSide::Input, static_cast<std::size_t>(input)) + " has only " +
              std::to_string(input_count_ - input - 1) + " after it";
        return false;
    }
    if (recording_fn_ && recording_fn_() && armed_fn_ && armed_fn_(static_cast<int>(index))) {
        why = t.name + " is recording a take from " +
              inputText(mix->input.front(), static_cast<int>(mix->input.size())).toStdString() +
              "; change its input after the take";
        return false;
    }
    TrackMix m = *mix;
    m.input.clear();
    for (int c = 0; c < channels; ++c) m.input.push_back(static_cast<std::uint16_t>(input + c));
    if (m.input == mix->input) return true;
    const bool ok = doc_.submit(SetMix{m}, why);
    session_.reportEdit(ok, why);
    refresh();
    return ok;
}

bool IoSetupModel::setTrackInput(int row, int input) {
    std::string why;
    return done(pickInput(row, input, why), why);
}

}  // namespace sonora::ui
