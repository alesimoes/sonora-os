// SPDX-License-Identifier: GPL-3.0-or-later
//
// Import Audio (ui.editing.import), as session.import_audio opens it over
// the Edit window: a folder of files written for the picture -- a stereo
// and a mono WAV at other sample rates (marked "converted to 48 kHz"), a
// file with six channels, a FLAC and a subfolder -- two of them chosen.
// The second picture is the refusal held in the panel when a chosen file
// has gone from the disk before Import.
#include <QDir>
#include <QFile>
#include <QQuickItem>
#include <cmath>
#include <vector>

#include "import_model.h"
#include "screenshot_scene.h"
#include "sonora/wav_writer.h"

namespace sonora::ui {
namespace {

void wav(const QString& path, uint32_t rate, double seconds, uint16_t channels) {
    WavWriter w;
    if (!w.open(path.toStdString(), rate, channels, WavWriter::Format::Int24, 0)) return;
    const std::size_t frames = static_cast<std::size_t>(seconds * rate);
    std::vector<float> buf(frames * channels, 0.0f);
    w.write(buf.data(), frames);
    w.close();
}

// The model is left showing the folder with the files.
ImportModel* prepare(ShotContext& c, QString* dir_out) {
    QDir dir(QDir::temp().filePath(QStringLiteral("sonora-scene-import")));
    dir.removeRecursively();
    QDir().mkpath(dir.path() + QStringLiteral("/session/Audio Files"));
    QDir().mkpath(dir.path() + QStringLiteral("/Stems from Dave/Drums"));
    const QString src = dir.path() + QStringLiteral("/Stems from Dave");
    wav(src + QStringLiteral("/Vocal take 3.wav"), 44100, 12.4, 2);
    wav(src + QStringLiteral("/Bass DI.wav"), 96000, 8.0, 1);
    wav(src + QStringLiteral("/Click.wav"), 48000, 3.0, 1);
    wav(src + QStringLiteral("/Room (5.1).wav"), 48000, 2.0, 6);
    QFile flac(src + QStringLiteral("/Reference mix.flac"));
    if (flac.open(QIODevice::WriteOnly)) flac.write("fLaC");
    c.session.setEngineFolder(dir.path() + QStringLiteral("/session"));
    c.model.invoke(QStringLiteral("session.import_audio"), -1);
    auto* m = static_cast<ImportModel*>(c.session.importer());
    m->open(src);
    *dir_out = src;
    return m;
}

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-import");
    s.take = false;
    s.apply = [](ShotContext& c) {
        QString src;
        ImportModel* m = prepare(c, &src);
        m->toggle(src + QStringLiteral("/Vocal take 3.wav"));
        m->toggle(src + QStringLiteral("/Bass DI.wav"));
    };
    return s;
}()};

const ShotRegistrar refused{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-import-refused");
    s.take = false;
    s.apply = [](ShotContext& c) {
        QString src;
        ImportModel* m = prepare(c, &src);
        m->toggle(src + QStringLiteral("/Click.wav"));
        QFile::remove(src + QStringLiteral("/Click.wav"));  // gone before Import
        m->startImport();
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
