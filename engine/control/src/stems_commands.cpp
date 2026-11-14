// SPDX-License-Identifier: GPL-3.0-or-later
//
// mix.stems (ADR-0085): refused at once with a reason, else started on a
// worker; how it ended rides EventKind::Bounce, as a bounce's does.
#include <memory>
#include <utility>

#include "sonora/command.h"
#include "sonora/stems_job.h"

namespace sonora {

namespace {

bool start_stems(StemsJob& job, const StemsCommandDeps& d, const std::string& argument,
                 std::string& why) {
    api::StemsOptions o;
    // Refuses mode=realtime with the contract's reason.
    if (!api::parse_stems_argument(argument, o, why)) return false;
    if (job.active() || (d.bounce != nullptr && d.bounce->exporting())) {
        why = "a bounce or a stem export is already running; cancel it or wait for it";
        return false;
    }
    if (d.recording && d.recording()) {
        why = "stop recording before exporting stems";
        return false;
    }
    const std::string dir = d.session_dir ? d.session_dir() : std::string();
    if (dir.empty()) {
        why = "the session has no folder to export stems into yet; save it first";
        return false;
    }
    const model::Snapshot now = d.snapshot();
    if (!stems_check(now, dir, o, why)) return false;
    job.start(now, dir, o);
    if (d.announce) d.announce(api::BounceState::Running, 0, 0, "");
    return true;
}

}  // namespace

bool install_stems_commands(CommandRegistry& registry, StemsJob& job, StemsCommandDeps deps) {
    auto d = std::make_shared<const StemsCommandDeps>(std::move(deps));
    return registry.install("mix.stems",
                            ExplainingHandler([&job, d](const CommandContext& ctx,
                                                        std::string& why) {
                                return start_stems(job, *d, ctx.argument, why);
                            }));
}

void follow_stems(StemsJob& job, const std::string& session_dir_now,
                  const StemsAnnounce& announce) {
    if (!job.active()) return;
    if (!job.finished()) {
        // A step in a hundred, as a bounce's.
        const uint32_t p = job.permille();
        if (p >= job.said() + 10) {
            job.said() = p;
            announce(api::BounceState::Running, p, 0, "");
        }
        return;
    }
    const StemsJob::Result r = job.collect();
    if (r.cancelled) {
        if (r.ok) remove_stems_folder(r.session_dir, r.stems.folder);
        announce(api::BounceState::Cancelled, 0, 0,
                 "the stem export was cancelled; no file was kept");
        return;
    }
    if (!r.ok) {
        announce(api::BounceState::Failed, 0, 0, r.error);
        return;
    }
    // Saved as another folder, or closed, while it rendered: the folder the
    // notice would name is not this session's.
    if (session_dir_now != r.session_dir) {
        remove_stems_folder(r.session_dir, r.stems.folder);
        announce(api::BounceState::Failed, 0, 0,
                 "the session moved while exporting stems; no file was kept");
        return;
    }
    announce(api::BounceState::Done, 1000, r.stems.files.size(), r.stems.folder);
}

}  // namespace sonora
