// SPDX-License-Identifier: GPL-3.0-or-later
//
// Import Session Data's contract (ADR-0123): the types a caller builds
// default to "nothing imported", and a refusal leaves the plan empty, so a
// caller that applies `plan.ops` after a refusal changes nothing.
#include <cstdio>

#include "sonora/session_files/session_import.h"

namespace sf = sonora::session_files;
namespace model = sonora::model;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

int types_default_to_nothing() {
    sf::SessionImportRequest request;
    CHECK(request.tracks.empty());
    CHECK(!request.markers);       // another song's markers only on request
    CHECK(request.offset == 0);
    CHECK(!request.take_open);
    CHECK(!request.source_readable);

    sf::ImportPlan plan;
    CHECK(plan.ops.ops.empty());
    CHECK(plan.copies.empty() && plan.dropped.empty() && plan.approximated.empty());

    sf::FileCopy copy;
    CHECK(copy.source == 0 && copy.source_path.empty() && copy.dest_path.empty());

    sf::ImportRefusal refusal;
    CHECK(refusal.reason == sf::ImportRefusal::Reason::None);
    CHECK(refusal.message.empty() && refusal.files.empty());

    sf::ImportableTrack row;
    CHECK(row.id == 0 && row.importable && row.why.empty());
    return 0;
}

int a_refusal_leaves_the_plan_empty() {
    model::SessionSettings settings;
    settings.name = "Dest";
    const model::Snapshot dest = model::new_session(settings);
    settings.name = "Source";
    const model::Snapshot source = model::new_session(settings);

    sf::SessionImportRequest request;
    request.tracks = {1};
    request.source_folder = "/sessions/Source";
    sf::ImportPlan plan;
    plan.dropped.push_back("left over from a previous call");
    sf::ImportRefusal refusal;
    CHECK(!sf::plan_session_import(dest, source, request, plan, refusal));
    CHECK(refusal.reason != sf::ImportRefusal::Reason::None);
    CHECK(!refusal.message.empty());
    CHECK(plan.ops.ops.empty() && plan.copies.empty() && plan.dropped.empty());
    return 0;
}

int an_empty_session_lists_no_track() {
    const model::Snapshot source = model::new_session(model::SessionSettings{});
    CHECK(sf::list_importable_tracks(source).empty());
    return 0;
}

}  // namespace

int main() {
    if (types_default_to_nothing() != 0) return 1;
    if (a_refusal_leaves_the_plan_empty() != 0) return 1;
    if (an_empty_session_lists_no_track() != 0) return 1;
    std::printf("session_import: ok\n");
    return 0;
}
