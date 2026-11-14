// SPDX-License-Identifier: GPL-3.0-or-later
//
// ShellModel's Export Stems (ADR-0085): mix.stems from the Bounce window's
// Stems section. Header-only and included at the end of shell_model.h. It
// shares the bounce's progress, cancel and held notice; it only remembers
// that the export it sent is stems, so Done reads as a folder.
#pragma once

#include <algorithm>

#include "shell_model.h"
#include "sonora/api/stems.h"

namespace sonora::ui {

inline bool ShellModel::stemsAvailable() const {
    const auto& caps = engine_.info().capabilities;
    return std::find(caps.begin(), caps.end(), api::kCapabilityStems) != caps.end();
}

inline void ShellModel::startStems(bool real_time, const QString& by, const QVariantList& track_ids,
                                   const QString& file, int bits, int rate, const QString& dither,
                                   const QString& name) {
    holdBounceRefusal(QString());
    api::StemsOptions o;
    o.by = by == QStringLiteral("output") ? api::StemsBy::Output : api::StemsBy::Track;
    for (const QVariant& id : track_ids) o.tracks.push_back(id.toULongLong());
    // Sent as asked: the engine refuses real time, in words the window holds.
    o.base.mode = real_time ? api::BounceMode::RealTime : api::BounceMode::Offline;
    o.base.format = bits == 32 ? api::BounceFormat::Float32
                    : bits == 16 ? api::BounceFormat::Int16 : api::BounceFormat::Int24;
    o.base.file = file == QStringLiteral("wav") ? api::BounceFile::Wav
                  : file == QStringLiteral("aiff") ? api::BounceFile::Aiff
                  : file == QStringLiteral("flac") ? api::BounceFile::Flac : api::BounceFile::Bwf;
    o.base.sample_rate = rate > 0 ? static_cast<uint32_t>(rate) : 0;
    o.base.dither = dither == QStringLiteral("none") ? api::BounceDither::None
                    : dither == QStringLiteral("tpdf") ? api::BounceDither::Tpdf
                    : dither == QStringLiteral("shaped") ? api::BounceDither::Shaped
                                                         : api::BounceDither::Auto;
    o.base.name = name.trimmed().toStdString();
    CommandContext ctx;
    ctx.argument = api::format_stems_argument(o);
    const RouteResult r = router_.invoke("mix.stems", ctx);
    report(r);
    if (r.kind == RouteResult::Kind::Sent) {
        // One export runs at a time: a second request is refused and must
        // not relabel the one under way.
        if (!bounceRunning()) bounce_is_stems_ = true;
    } else if (!r.message.empty()) {
        holdBounceRefusal(QString::fromStdString(r.message));
    }
}

}  // namespace sonora::ui
