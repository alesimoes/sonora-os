// SPDX-License-Identifier: GPL-3.0-or-later
//
// "tempo.edit detect": find the beats of the selected clip and set the
// session's tempo map from them (ADR-0114). The detection sits behind
// BeatRunner so tests use the analyze fake and production runs
// sonora-analyze; the map goes through EditController::submit_tempo, so
// Undo Tempo reverts it.
#pragma once

#include <functional>
#include <string>

#include "sonora/analyze/beat_detector.h"
#include "sonora/model/model.h"

namespace sonora::ui {

class EditController;

class BeatRunner {
public:
    virtual ~BeatRunner() = default;
    // `source` is the selected region's first channel source. False with
    // `error`; a detection is never guessed.
    virtual bool run(model::Id source, analyze::BeatResult& out, std::string& error) = 0;
};

// Runs `sonora-analyze beat <file> --out <report>` and reads the report
// strictly. `file_of` maps a source id to its file ("" when unknown);
// `report_path` is a scratch file the caller owns.
class ProcessBeatRunner final : public BeatRunner {
public:
    ProcessBeatRunner(std::string analyze_exe, std::function<std::string(model::Id)> file_of,
                      std::string report_path)
        : exe_(std::move(analyze_exe)), file_of_(std::move(file_of)), report_(std::move(report_path)) {}
    bool run(model::Id source, analyze::BeatResult& out, std::string& error) override;

private:
    std::string exe_;
    std::function<std::string(model::Id)> file_of_;
    std::string report_;
};

// "detect [clip|selection] [follow]". "follow" (a map entry per bar) is
// refused for a stable result. Refuses below kBeatMinConfidence with the map
// unchanged. A null runner refuses: nothing here can reach the analyzer.
bool beat_command(EditController& ec, BeatRunner* runner, const std::string& text, std::string& why);

// The tempo.edit entry: no runner reachable from there yet.
bool beat_command(EditController& ec, const std::string& text, std::string& why);

}  // namespace sonora::ui
