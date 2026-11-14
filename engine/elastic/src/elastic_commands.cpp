// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/elastic/elastic_commands.h"

#include <utility>

#include "sonora/command.h"
#include "sonora/session_files/session_folder.h"

namespace sonora::elastic {

namespace {
constexpr const char* kKept = "; nothing was changed";
constexpr const char* kCommand = "audio.elastic";
}

ElasticService::ElasticService(StretcherFactory stretcher, std::unique_ptr<OnsetSource> onsets,
                               ElasticCommandDeps deps)
    : deps_(std::move(deps)), job_(std::move(stretcher), std::move(onsets)) {}

bool ElasticService::start(const std::string& argument, std::string& why) {
    api::ElasticRequest request;
    if (!api::parse_elastic_request(argument, request, why)) return false;
    if (job_.active()) {
        why = "a region is already being stretched; cancel it or wait for it";
        return false;
    }
    // A take owns the playlists while it is open (structure_lock).
    if (deps_.recording && deps_.recording()) {
        why = "stop recording before stretching audio; nothing was stretched";
        return false;
    }
    const std::string dir = deps_.session_dir ? deps_.session_dir() : std::string();
    if (dir.empty()) {
        why = "the session has no folder for the stretched file yet; save it first";
        return false;
    }
    ElasticPlan plan;
    if (!elastic_check(deps_.document->current(), request, plan, why)) return false;
    if (!model_accepts(plan, why)) return false;
    track_ = plan.track;
    job_.start(std::move(plan), dir);
    if (deps_.announce) deps_.announce(api::RenderState::Running, 0, track_, "");
    return true;
}

// Asked of the model first with the copy's size, so a refusal costs no
// render. Quantize's length is the onsets'; its edit is asked once placed.
bool ElasticService::model_accepts(const ElasticPlan& plan, std::string& why) const {
    if (plan.request.mode == api::ElasticMode::Quantize) return true;
    ElasticOutput probe;
    probe.markers = plan.markers;
    probe.region_length = api::elastic_output_frames(plan.request, plan.region.length);
    probe.source.path = std::string(session_files::kAudioDir) + "/pending.wav";
    probe.source.channels = static_cast<std::uint16_t>(plan.files.size());
    probe.source.length = plan.lead + probe.region_length + plan.tail;
    probe.source.format = plan.format;
    probe.source.kind = model::SourceKind::Imported;
    const model::Snapshot now = deps_.document->current();
    model::Compound c;
    model::Snapshot after;
    if (!elastic_compound(now, plan, probe, c, why) || !model::apply(now, c, after, why)) {
        why = "the session would refuse the stretch: " + why + "; nothing was stretched";
        return false;
    }
    return true;
}

bool ElasticService::cancel() {
    if (!job_.active()) return false;
    job_.cancel();
    return true;
}

void ElasticService::follow() {
    if (!job_.active()) return;
    if (!job_.finished()) {
        // A step in a hundred: a long render is not a flood of events.
        const std::uint32_t p = job_.permille();
        if (p >= job_.said() + 10) {
            job_.said() = p;
            deps_.announce(api::RenderState::Running, p, track_, "");
        }
        return;
    }
    ElasticJob::Result r = job_.collect();
    if (r.cancelled) {
        remove_copy(r);
        deps_.announce(api::RenderState::Cancelled, 0, track_,
                       std::string("the stretch was cancelled") + kKept);
        return;
    }
    if (!r.ok) {
        deps_.announce(api::RenderState::Failed, 0, track_, r.error + kKept);
        return;
    }
    const std::string dir = deps_.session_dir ? deps_.session_dir() : std::string();
    if (dir != r.session_dir) {
        remove_copy(r);
        deps_.announce(api::RenderState::Failed, 0, track_,
                       std::string("the session moved while the region was stretched") + kKept);
        return;
    }
    if (deps_.recording && deps_.recording()) {
        remove_copy(r);
        deps_.announce(api::RenderState::Failed, 0, track_,
                       std::string("a take started while the region was stretched") + kKept);
        return;
    }
    place(std::move(r));
}

void ElasticService::place(ElasticJob::Result r) {
    std::string why;
    model::Compound c;
    model::ReplicationUpdate u;
    if (!elastic_compound(deps_.document->current(), r.plan, r.output, c, why)) {
        remove_copy(r);
        deps_.announce(api::RenderState::Failed, 0, track_, why);
        return;
    }
    if (!deps_.document->apply(c, u, why)) {
        remove_copy(r);
        deps_.announce(api::RenderState::Failed, 0, track_,
                       "the session refused the stretched file: " + why + kKept);
        return;
    }
    if (deps_.changed) deps_.changed({u});
    deps_.announce(api::RenderState::Done, 1000, track_, r.output.source.path);
}

void ElasticService::abandon() {
    if (!job_.abandon()) return;
    deps_.announce(api::RenderState::Cancelled, 0, track_,
                   "the session was closed; the stretch was stopped");
}

bool install_elastic_commands(CommandRegistry& registry, std::unique_ptr<ElasticService>& out,
                              StretcherFactory stretcher, std::unique_ptr<OnsetSource> onsets,
                              ElasticCommandDeps deps) {
    out.reset();
    if (!stretcher) return true;
    if (find_command(kCommand) == nullptr) return false;
    out = std::make_unique<ElasticService>(std::move(stretcher), std::move(onsets),
                                           std::move(deps));
    ElasticService* service = out.get();
    return registry.install(kCommand,
                            ExplainingHandler([service](const CommandContext& ctx,
                                                        std::string& why) {
                                return service->start(ctx.argument, why);
                            }));
}

}  // namespace sonora::elastic
