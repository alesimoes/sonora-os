// SPDX-License-Identifier: GPL-3.0-or-later
//
// audio.elastic on the control server (ADR-0136 §2): refused at once with a
// reason, else rendered on a worker (ElasticJob) and placed by one Compound
// on the control thread. Progress and the outcome ride EventKind::Render,
// as a render in place's do, and track.render_cancel stops it.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "sonora/api/render.h"
#include "sonora/elastic/elastic_job.h"
#include "sonora/model/replication.h"

namespace sonora {
class CommandRegistry;
}

namespace sonora::elastic {

// The Render event, as the server posts it: the track it is about.
using ElasticAnnounce = std::function<void(api::RenderState state, std::uint32_t permille,
                                           model::Id track, const std::string& text)>;

// What audio.elastic reads of the server, injected so a test drives it alone.
struct ElasticCommandDeps {
    model::Document* document = nullptr;            // the open document, outliving this
    std::function<std::string()> session_dir;      // empty: no folder yet
    std::function<bool()> recording;               // a take is open or asked for
    // The engine applied `updates`: replicas, chains and plan follow.
    std::function<void(const std::vector<model::ReplicationUpdate>&)> changed;
    ElasticAnnounce announce;
};

class ElasticService {
public:
    ElasticService(StretcherFactory stretcher, std::unique_ptr<OnsetSource> onsets,
                   ElasticCommandDeps deps);

    // audio.elastic's handler: false, with the reason, for a refusal.
    bool start(const std::string& argument, std::string& why);
    bool active() const { return job_.active(); }
    // True when a render was running to cancel.
    bool cancel();
    // Each control-thread turn: progress, then the copy into the document.
    void follow();
    // The session closed: stopped, its copy removed, Cancelled said.
    void abandon();

private:
    bool model_accepts(const ElasticPlan& plan, std::string& why) const;
    void place(ElasticJob::Result result);
    ElasticCommandDeps deps_;
    ElasticJob job_;
    model::Id track_ = 0;   // of the render running, for the Render event
};

// Installs audio.elastic, building `out`, when `stretcher` is not empty;
// without one the command stays unimplemented and `out` null, so neither it
// nor the capability is announced. False only when the catalog lacks it.
bool install_elastic_commands(CommandRegistry& registry, std::unique_ptr<ElasticService>& out,
                              StretcherFactory stretcher, std::unique_ptr<OnsetSource> onsets,
                              ElasticCommandDeps deps);

}  // namespace sonora::elastic
