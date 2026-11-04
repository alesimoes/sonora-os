// SPDX-License-Identifier: GPL-3.0-or-later
//
// The mix assistant's rule-based planners (ADR-0045, on the plan contract of
// ADR-0038): gain staging, bus structure, parallel compression and
// saturation. No model, no randomness: the same session and the same
// analysis give the same plan, and every step says which rule and which
// measurement is behind it.
//
// The analysis pass reads the session's audio on the control thread or a
// worker: it opens files and allocates. Never the audio thread.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "sonora/model/model.h"
#include "sonora/model/plan.h"

namespace sonora {

// --- analysis ----------------------------------------------------------------

// What one track's active playlist measures, before its fader and pan: the
// regions as they play (region gain applied, muted regions left out), one
// after the other.
struct TrackAnalysis {
    model::Id track = 0;
    model::Frames frames = 0;     // frames measured
    double peak_dbfs = -200.0;    // sample peak over the channels
    double true_peak_dbtp = -200.0;
    double rms_dbfs = -200.0;     // over the measured frames, all channels
    double lufs = -200.0;         // BS.1770 integrated (gated)
    // Empty, or why the track was not measured ("Audio Files/x.wav: ...").
    std::string problem;
    bool silent() const { return frames == 0 || peak_dbfs < -80.0; }
    double crest_db() const { return peak_dbfs - rms_dbfs; }
};

struct MixAnalysis {
    std::vector<TrackAnalysis> tracks;   // in the session's track order
    const TrackAnalysis* find(model::Id track) const;
};

struct AnalysisHooks {
    // The fraction measured, in (0, 1], after each block; calling thread.
    std::function<void(double)> progress;
    // Polled once per block; set it from another thread to stop.
    const std::atomic<bool>* cancel = nullptr;
};

// False, with `error`, only when cancelled. A file that cannot be read costs
// that one track (TrackAnalysis::problem), as a bad take does.
bool analyse_session(const model::Snapshot& snapshot, const std::string& session_dir,
                     const AnalysisHooks& hooks, MixAnalysis& out, std::string& error);

// --- roles ---------------------------------------------------------------------

enum class Role { Unknown, Drums, Bass, Guitars, Keys, Vocals };
const char* role_name(Role role);

struct RoleGuess {
    Role role = Role::Unknown;
    std::string why;   // "name \"Kick In\" matches \"kick\""
};
// From the track's name, then its plugin inserts, then the colour (not the
// default 0) it shares with a track of `edit` that the name classifies.
RoleGuess classify_track(const model::EditState& edit, const model::TrackEdit& track);

// --- planners -------------------------------------------------------------------

struct GainStagingOptions {
    // Where a track's integrated loudness sits after its fader.
    double target_lufs = -23.0;
    // No track's true peak, after its fader, above this.
    double ceiling_dbtp = -3.0;
    // The estimated master peak (uncorrelated tracks: powers add) stays under
    // minus this; a common trim on every track brings it there.
    double master_headroom_db = 6.0;
    // A smaller change is not worth a step.
    double min_change_db = 1.0;
};

// Trims faders (SetMix) so every track sits at the target with headroom on
// the master. Faders are the only thing it touches.
class GainStagingPlanner : public model::PlanSource {
public:
    GainStagingPlanner(const MixAnalysis& analysis, GainStagingOptions options = {})
        : analysis_(analysis), options_(options) {}
    bool propose(const model::Snapshot& at, model::Plan& out, std::string& error) override;

private:
    MixAnalysis analysis_;
    GainStagingOptions options_;
};

struct BusStructureOptions {
    // A role with fewer tracks gets no bus.
    std::size_t min_tracks = 2;
};

// An aux bus per role (AddBus) and the role's tracks routed to it
// (SetOutput). A bus of that name that exists is reused.
class BusStructurePlanner : public model::PlanSource {
public:
    explicit BusStructurePlanner(BusStructureOptions options = {}) : options_(options) {}
    bool propose(const model::Snapshot& at, model::Plan& out, std::string& error) override;

private:
    BusStructureOptions options_;
};

// A processor and its settings, by the LV2 URI and the parameter keys of our
// native suite (plugins/native), ready to become an AddInsert.
struct ProcessorSetting {
    std::string uri;    // "urn:sonora:native:compressor"
    std::string name;   // "Sonora Compressor"
    std::vector<model::ParamValue> params;
};

// One parallel aux the planner proposes: the sends are steps of the plan, the
// processor is data, because aux buses cannot hold inserts yet (ADR-0045 5).
struct ParallelSuggestion {
    std::string bus_name;
    ProcessorSetting processor;
};

// A parallel-compression aux for the drums and a saturation aux for the
// bass: the bus (AddBus), a send per track (AddSend) at a level taken from
// the measurements, and the processor to put on the aux, as a PlanNote and
// in suggestions().
class ParallelPlanner : public model::PlanSource {
public:
    explicit ParallelPlanner(const MixAnalysis& analysis) : analysis_(analysis) {}
    bool propose(const model::Snapshot& at, model::Plan& out, std::string& error) override;
    const std::vector<ParallelSuggestion>& suggestions() const { return suggestions_; }

private:
    MixAnalysis analysis_;
    std::vector<ParallelSuggestion> suggestions_;
};

enum class MixPlanKind { Gain, Buses, Parallel, All };
bool parse_plan_kind(const std::string& word, MixPlanKind& out);

// Runs the planner(s) one after the other on the same snapshot and joins the
// plans: ids are carried on, `needs` re-indexed. "All" is gain staging, then
// buses, then parallel. Fails with `error` when no planner has a step.
bool propose_mix_plan(MixPlanKind kind, const model::Snapshot& at, const MixAnalysis& analysis,
                      model::Plan& out, std::string& error);

// The text preview: each step with its reason and dependencies, the notes,
// then the diff accepting it all would make. False, with `error`, when the
// plan does not preview.
bool format_plan(const model::Snapshot& at, const model::Plan& plan, std::string& text,
                 std::string& error);

}  // namespace sonora
