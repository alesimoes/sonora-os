// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pitch (time.pitch, ADR-0143). A sung phrase, synthesised here with vibrato
// and drift, is tracked and segmented by common/pitch as a real region would
// be, and the Note Pitch panel is opened over the Edit window on it:
//   "edit-pitch"          the third note pulled onto A in E minor, selected,
//                         its drift straightened and its vibrato narrowed;
//   "edit-pitch-refused"  room noise analysed: unpitched, refused, the reason
//                         held in the notice band;
//   "edit-pitch-off"      the engine's own Hello read: the demo's fake engine
//                         has no shifter, so the editor is replaced by why.
// The first two state the capability as an engine built with a shifter
// announces it; the demo engine cannot, and the third shows what it says.
//
// The panel is not reachable from the Edit window yet (that hook is the
// follow-up, ADR-0143 Consequences); these scenes open it directly.

// Only where ui/pitch is configured (the Linux build), as groove.cpp does.
#if __has_include("sonora/ui_pitch/pitch_panel.h")
#include "sonora/ui_pitch/pitch_editor_model.h"
#include "sonora/ui_pitch/pitch_panel.h"
#include "sonora/ui_pitch/pitch_view.h"
#define SONORA_UI_PITCH_SCENE 1
#endif

#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QUrl>
#include <QVariantMap>

#include <cmath>

#include "screenshot_scene.h"

#ifdef SONORA_UI_PITCH_SCENE
namespace sonora::ui {
namespace {

using ui_pitch::PitchEditorModel;

constexpr uint32_t kRate = 48000;
constexpr double kTwoPi = 6.283185307179586;

// E4 G4 A4 G4 E4 D4, sung: a scoop into each note, 5.5 Hz vibrato, the A
// 35 cents flat and drifting down, short breaths between some notes.
std::vector<float> sung_phrase() {
    struct Sung { double cents, seconds, gap, drift; };
    const Sung phrase[] = {{-500, 0.42, 0.05, 0}, {-200, 0.38, 0.00, 10}, {-35, 0.62, 0.06, -30},
                           {-200, 0.36, 0.00, 0}, {-488, 0.44, 0.07, 0}, {-700, 0.56, 0.00, 15}};
    std::vector<float> out;
    double phase = 0.0;
    for (const Sung& s : phrase) {
        const std::size_t n = std::size_t(s.seconds * kRate);
        for (std::size_t i = 0; i < n; ++i) {
            const double t = double(i) / kRate;
            const double scoop = -60.0 * std::exp(-t / 0.03);
            const double cents = s.cents + scoop + s.drift * t / s.seconds +
                                 22.0 * std::sin(kTwoPi * 5.5 * t);
            phase += kTwoPi * 440.0 * std::exp2(cents / 1200.0) / kRate;
            out.push_back(float(0.4 * std::sin(phase)));
        }
        out.insert(out.end(), std::size_t(s.gap * kRate), 0.0f);
    }
    return out;
}

std::vector<float> room_noise() {
    std::vector<float> out(2 * kRate);
    uint32_t x = 12345;
    for (float& v : out) {
        x = x * 1664525u + 1013904223u;
        v = 0.2f * (float(x >> 8) / float(1u << 24) - 0.5f);
    }
    return out;
}

ui_pitch::PitchRegion region_for(const std::vector<float>& audio, const char* name) {
    ui_pitch::PitchRegion r;
    r.id = 1;
    r.length = audio.size();
    r.rate = kRate;
    r.name = name;
    return r;
}

QVariantList note_list(const std::vector<ui_pitch::NoteBlob>& blobs) {
    QVariantList out;
    for (const ui_pitch::NoteBlob& b : blobs) {
        QVariantList contour;
        for (const double v : b.contour) contour.push_back(v);
        QVariantMap m;
        m.insert(QStringLiteral("x"), b.x);
        m.insert(QStringLiteral("y"), b.y);
        m.insert(QStringLiteral("w"), b.w);
        m.insert(QStringLiteral("h"), b.h);
        m.insert(QStringLiteral("originY"), b.origin_y);
        m.insert(QStringLiteral("moved"), b.moved);
        m.insert(QStringLiteral("selected"), b.selected);
        m.insert(QStringLiteral("label"), QString::fromStdString(b.label));
        m.insert(QStringLiteral("contour"), contour);
        out.push_back(m);
    }
    return out;
}

QVariantList row_list(const std::vector<ui_pitch::RulerRow>& rows) {
    QVariantList out;
    for (const ui_pitch::RulerRow& r : rows) {
        QVariantMap m;
        m.insert(QStringLiteral("y"), r.y);
        m.insert(QStringLiteral("h"), r.h);
        m.insert(QStringLiteral("name"), QString::fromStdString(r.name));
        m.insert(QStringLiteral("black"), r.black);
        out.push_back(m);
    }
    return out;
}

QVariantMap inspector(const PitchEditorModel& m) {
    QVariantMap out;
    if (m.selected() < 0) {
        out.insert(QStringLiteral("none"), true);
        return out;
    }
    const std::size_t i = std::size_t(m.selected());
    const auto named = [](double cents) {
        return QString::fromStdString(ui_pitch::note_name(cents) + " " + ui_pitch::cents_off_label(cents));
    };
    out.insert(QStringLiteral("name"), named(m.pitch_cents(i)));
    out.insert(QStringLiteral("found"), named(m.note(i).median_cents));
    out.insert(QStringLiteral("shift"), m.edit(i).shift_cents);
    out.insert(QStringLiteral("straighten"), 100.0 * (1.0 - m.edit(i).drift_scale));
    out.insert(QStringLiteral("vibrato"), 100.0 * m.edit(i).vibrato_scale);
    out.insert(QStringLiteral("formant"), m.edit(i).formant_cents);
    out.insert(QStringLiteral("gain"), m.edit(i).gain_db);
    return out;
}

// Everything the panel draws, from the model, for a roll of w x h pixels.
void show_model(QQuickItem* panel, const PitchEditorModel& m) {
    const double w = panel->property("canvasWidth").toDouble();
    const double h = panel->property("canvasHeight").toDouble();
    const ui_pitch::PitchViewport v = ui_pitch::fit_viewport(m, w, h);
    QStringList keys, scales;
    for (int k = 0; k < 12; ++k) keys << QString::fromStdString(ui_pitch::key_name(k));
    for (int s = 0; s <= int(pitch::Scale::MinorPentatonic); ++s) {
        scales << QString::fromStdString(ui_pitch::scale_name(pitch::Scale(s)));
    }
    const bool can_edit = m.is_open() && !m.pending() && !m.stale();
    panel->setProperty("available", m.capability().available);
    panel->setProperty("reason", QString::fromStdString(m.capability().reason));
    panel->setProperty("regionName", QString::fromStdString(m.region().name));
    panel->setProperty("status", QString::fromStdString(m.status()));
    panel->setProperty("notice", QString::fromStdString(m.notice()));
    panel->setProperty("rows", row_list(ui_pitch::ruler_rows(v)));
    panel->setProperty("notes", note_list(ui_pitch::note_blobs(m, v)));
    panel->setProperty("inspector", inspector(m));
    panel->setProperty("keyNames", keys);
    panel->setProperty("scaleNames", scales);
    panel->setProperty("snapMode", int(m.snap_mode()));
    panel->setProperty("key", m.key());
    panel->setProperty("scale", int(m.scale()));
    panel->setProperty("amount", m.amount());
    panel->setProperty("canEdit", can_edit);
    panel->setProperty("canApply", can_edit && m.capability().available && m.edited());
}

void open_panel(ShotContext& c, const PitchEditorModel& m) {
    QQmlComponent comp(c.view.engine());
    comp.setData(QByteArray(ui_pitch::pitch_panel_qml()),
                 QUrl(QString::fromLatin1(ui_pitch::kPitchPanelUrl)));
    QObject* o = comp.create();
    auto* panel = qobject_cast<QQuickItem*>(o);
    if (panel == nullptr) {
        std::fprintf(stderr, "sonora-ui: pitch scene: %s\n", qPrintable(comp.errorString()));
        delete o;
        return;
    }
    panel->setParent(c.view.rootObject());
    panel->setParentItem(c.view.rootObject());
    show_model(panel, m);
    QMetaObject::invokeMethod(panel, "show");
}

void complain(const std::string& why) {
    std::fprintf(stderr, "sonora-ui: pitch scene: %s\n", why.c_str());
}

void edited_phrase(ShotContext& c) {
    PitchEditorModel m;
    m.set_capability({true, ""});
    std::string why;
    pitch::YinTracker yin;
    const std::vector<float> audio = sung_phrase();
    if (!m.analyse(region_for(audio, "Lead Vocal_03"), audio, yin, why)) return complain(why);
    if (!m.set_key(4, why)) return complain(why);  // E minor
    m.set_scale(pitch::Scale::NaturalMinor);
    m.set_snap_mode(ui_pitch::SnapMode::Scale);
    // The flat A: dragged up a little, the scale snap lands it on A.
    const int a = m.size() > 2 ? 2 : int(m.size()) - 1;
    if (!m.begin_drag(a, why) || !m.drag_to(25.0, why)) return complain(why);
    m.end_drag();
    if (!m.set_straighten(60.0, why) || !m.set_vibrato(70.0, why)) return complain(why);
    open_panel(c, m);
}

void refused_noise(ShotContext& c) {
    PitchEditorModel m;
    m.set_capability({true, ""});
    std::string why;
    pitch::YinTracker yin;
    const std::vector<float> audio = room_noise();
    if (m.analyse(region_for(audio, "Room Tone_01"), audio, yin, why)) {
        complain("room noise was not refused");
    }
    open_panel(c, m);
}

void engine_without_shifter(ShotContext& c) {
    PitchEditorModel m;
    const EngineInfo& info = c.model.engineAdapter().info();
    m.set_capability(ui_pitch::pitch_capability(info.implemented_commands, info.capabilities));
    open_panel(c, m);
}

ShotScene scene(const char* name, void (*apply)(ShotContext&)) {
    ShotScene s;
    s.name = QString::fromLatin1(name);
    s.take = false;
    s.apply = apply;
    return s;
}

const ShotRegistrar edited{scene("edit-pitch", edited_phrase)};
const ShotRegistrar refused{scene("edit-pitch-refused", refused_noise)};
const ShotRegistrar off{scene("edit-pitch-off", engine_without_shifter)};

}  // namespace
}  // namespace sonora::ui
#endif
