// SPDX-License-Identifier: GPL-3.0-or-later
#include "track_view_model.h"

#include <QVariantMap>

namespace sonora::ui {

namespace {

constexpr const char* kPresets = "small, medium, large or jumbo";

std::string preset_help(const std::string& given) {
    return "\"" + given + "\" is not a track height; use " + kPresets + ". No height changed.";
}

}  // namespace

TrackViewModel::TrackViewModel(TrackViewStore& store, TrackViewSource source, QObject* parent)
    : QObject(parent), store_(store), src_(std::move(source)) {}

void TrackViewModel::installCommands(CommandRegistry& local) {
    local.install("window.tracks_list", [this](const CommandContext&) {
        setOpen(!open_);
        return true;
    });
    local.install("track.height",
                  ExplainingHandler([this](const CommandContext& c, std::string& why) {
                      if (!c.has_target()) {
                          why = "select a track first, then give its height: " +
                                std::string(kPresets);
                          return false;
                      }
                      return setHeight(c.target, c.argument, why);
                  }));
    local.install("track.hide", ExplainingHandler([this](const CommandContext& c, std::string& why) {
                      if (!c.has_target()) {
                          why = "select the track to hide first";
                          return false;
                      }
                      const bool ok = hide(c.target, why);
                      // A track kept on screen because it is armed is a refusal
                      // the operator must see, not a status line that fades.
                      if (!ok && !why.empty()) hold(why);
                      return ok;
                  }));
    local.install("track.show_all",
                  ExplainingHandler([this](const CommandContext&, std::string& why) {
                      return showAll(why);
                  }));
}

void TrackViewModel::setOpen(bool on) {
    if (on == open_) return;
    open_ = on;
    emit openChanged();
}

void TrackViewModel::checkSession() const {
    const std::string id = src_.session_id();
    if (id == seen_session_) return;
    seen_session_ = id;
    notice_.clear();
}

QString TrackViewModel::notice() const {
    checkSession();
    return notice_;
}

void TrackViewModel::dismissNotice() {
    if (notice_.isEmpty()) return;
    notice_.clear();
    emit noticeChanged();
}

void TrackViewModel::hold(const std::string& why) {
    checkSession();
    notice_ = QString::fromStdString(why);
    emit noticeChanged();
    emit heldNotice(notice_);
}

void TrackViewModel::touched() {
    ++generation_;
    emit changed();
}

bool TrackViewModel::armed(int index) const { return src_.armed && src_.armed(index); }

TrackView TrackViewModel::view(int index) const {
    return store_.get(src_.session_id(), static_cast<std::uint32_t>(src_.track_id(index)));
}

void TrackViewModel::write(int index, const TrackView& v) {
    store_.set(src_.session_id(), static_cast<std::uint32_t>(src_.track_id(index)), v);
    touched();
}

bool TrackViewModel::valid(int index, std::string& why) const {
    if (index >= 0 && index < src_.count()) return true;
    why = "that track does not exist";
    return false;
}

bool TrackViewModel::active() const {
    checkSession();
    const std::string id = src_.session_id();
    const int n = src_.count();
    if (id == cache_session_ && n == cache_count_ && generation_ == cache_generation_) {
        return cache_active_;
    }
    cache_active_ = false;
    for (int i = 0; i < n && !cache_active_; ++i) {
        const TrackView v = view(i);
        cache_active_ = v.hidden || v.height != TrackHeight::Medium;
    }
    cache_session_ = id;
    cache_count_ = n;
    cache_generation_ = generation_;
    return cache_active_;
}

RowSpec TrackViewModel::row(int index, std::uint64_t track) const {
    const TrackView v = store_.get(src_.session_id(), static_cast<std::uint32_t>(track));
    RowSpec s;
    s.scale = height_scale(v.height);
    // Whatever the store says, an armed track stays on screen.
    s.hidden = v.hidden && !armed(index);
    return s;
}

bool TrackViewModel::setHeight(int index, const std::string& preset, std::string& why) {
    TrackHeight h;
    if (!valid(index, why)) return false;
    if (!parse_track_height(preset, h)) {
        why = preset_help(preset);
        return false;
    }
    TrackView v = view(index);
    v.height = h;
    write(index, v);
    return true;
}

bool TrackViewModel::hide(int index, std::string& why) {
    if (!valid(index, why)) return false;
    if (armed(index)) {
        why = src_.name(index) +
              " is record-armed and stays visible; disarm it to hide it. Nothing was hidden.";
        return false;
    }
    TrackView v = view(index);
    v.hidden = true;
    write(index, v);
    return true;
}

bool TrackViewModel::show(int index) {
    std::string why;
    if (!valid(index, why)) return false;
    TrackView v = view(index);
    if (!v.hidden) return true;
    v.hidden = false;
    write(index, v);
    return true;
}

bool TrackViewModel::showAll(std::string& why) {
    checkSession();
    if (store_.show_all(src_.session_id()) == 0) {
        why = "no track is hidden";
        return false;
    }
    touched();
    return true;
}

bool TrackViewModel::setHeightHeld(int index, const QString& preset) {
    std::string why;
    const bool ok = setHeight(index, preset.toStdString(), why);
    if (!ok) hold(why);
    return ok;
}

bool TrackViewModel::setHiddenHeld(int index, bool hidden) {
    std::string why;
    const bool ok = hidden ? hide(index, why) : show(index);
    if (!ok && !why.empty()) hold(why);
    return ok;
}

bool TrackViewModel::showAllHeld() {
    std::string why;
    const bool ok = showAll(why);
    if (!ok) hold(why);
    return ok;
}

int TrackViewModel::hiddenCount() const {
    int n = 0;
    for (int i = 0; i < src_.count(); ++i) n += view(i).hidden ? 1 : 0;
    return n;
}

QVariantList TrackViewModel::rows() const {
    checkSession();
    QVariantList out;
    for (int i = 0; i < src_.count(); ++i) {
        const TrackView v = view(i);
        QVariantMap m;
        m.insert(QStringLiteral("index"), i);
        m.insert(QStringLiteral("name"), QString::fromStdString(src_.name(i)));
        m.insert(QStringLiteral("height"), QString::fromLatin1(track_height_name(v.height)));
        m.insert(QStringLiteral("hidden"), v.hidden);
        m.insert(QStringLiteral("armed"), armed(i));
        out.append(m);
    }
    return out;
}

}  // namespace sonora::ui
