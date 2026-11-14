// SPDX-License-Identifier: GPL-3.0-or-later
//
// audio.elastic through the command registry (ADR-0136 §2, §3): not installed
// without a stretcher, refused while a take is open, rendered on the worker
// and placed in one undo step, and on cancel, on a session closed, or on an
// edit or a take that lands meanwhile, no copy is left and nothing changes.
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "elastic_rig.h"
#include "sonora/command.h"
#include "sonora/elastic/elastic_commands.h"
#include "sonora/model/operations.h"

namespace {

using namespace sonora;
using namespace elastic_test;

constexpr const char* kCommand = "audio.elastic";

// Holds the render until the test lets it go or cancels it: what lands
// meanwhile is then certain to land mid-render, with no clock involved.
class GateStretcher final : public stretch::Stretcher {
public:
    GateStretcher(const std::atomic<bool>& release, const std::atomic<bool>& cancel)
        : release_(release), cancel_(cancel) {}
    bool process(const stretch::Planar& in, const stretch::WarpMap& map, stretch::Algorithm a,
                 stretch::Planar& out, std::string& why) override {
        while (!release_.load() && !cancel_.load()) std::this_thread::yield();
        if (cancel_.load()) {
            why = "cancelled";
            return false;
        }
        return stretch::LinearResampleStretcher().process(in, map, a, out, why);
    }

private:
    const std::atomic<bool>& release_;
    const std::atomic<bool>& cancel_;
};

struct Harness {
    Rig rig;
    CommandRegistry registry;
    std::unique_ptr<elastic::ElasticService> service;
    std::atomic<bool> release{false};
    bool recording = false;
    std::string folder;
    std::vector<std::pair<api::RenderState, std::string>> said;
    int changed = 0;

    bool install(bool with_library) {
        folder = rig.dir;
        elastic::ElasticCommandDeps d;
        d.document = rig.doc.get();
        d.session_dir = [this]() { return folder; };
        d.recording = [this]() { return recording; };
        d.changed = [this](const std::vector<model::ReplicationUpdate>& u) {
            changed += static_cast<int>(u.size());
        };
        d.announce = [this](api::RenderState s, std::uint32_t, model::Id, const std::string& t) {
            said.emplace_back(s, t);
        };
        elastic::StretcherFactory f;
        if (with_library) {
            f = [this](std::uint32_t, const std::atomic<bool>& cancel) {
                return std::unique_ptr<stretch::Stretcher>(new GateStretcher(release, cancel));
            };
        }
        return elastic::install_elastic_commands(registry, service, std::move(f), nullptr,
                                                 std::move(d));
    }

    DispatchResult run(const std::string& rest, std::string& why) {
        CommandContext ctx;
        ctx.argument = std::to_string(rig.region) + " " + rest;
        return registry.dispatch(kCommand, ctx, why);
    }

    // Follows the render to its end, as the server's turns do.
    void finish() {
        while (service->active()) {
            service->follow();
            std::this_thread::yield();
        }
    }
};

bool make(Harness& h, const char* dir, bool with_library = true) {
    return make_rig(h.rig, dir, 288000, 48000, 24000, 120000) && h.install(with_library);
}

int test_absent_without_library() {
    Harness h;
    CHECK(make(h, "test_elastic_cmd_none", false));
    CHECK(h.service == nullptr && !h.registry.implemented(kCommand));
    std::string why;
    CHECK(h.run("polyphonic ratio=2", why) == DispatchResult::NotImplemented);
    return 0;
}

int test_apply_and_undo() {
    Harness h;
    CHECK(make(h, "test_elastic_cmd_apply"));
    CHECK(h.service != nullptr && h.registry.implemented(kCommand));
    const std::string before = slurp(h.rig.take());
    const std::size_t undo = h.rig.doc->history().undo_depth();
    std::string why;
    h.release.store(true);
    CHECK(h.run("polyphonic ratio=2", why) == DispatchResult::Ok);
    CHECK(!h.said.empty() && h.said.front().first == api::RenderState::Running);
    h.finish();
    CHECK(h.said.back().first == api::RenderState::Done);
    CHECK(h.said.back().second == "Audio Files/Drums_elastic_01.wav");
    CHECK(h.changed == 1 && h.rig.doc->history().undo_depth() == undo + 1);
    CHECK(region_of(h.rig, h.rig.region)->length == 48000);
    CHECK(slurp(h.rig.take()) == before);
    model::ReplicationUpdate u;
    CHECK(h.rig.doc->undo(u, why));
    CHECK(source_of(h.rig, h.rig.region)->id == h.rig.source);
    CHECK(region_of(h.rig, h.rig.region)->length == 24000);
    CHECK(slurp(h.rig.take()) == before);
    return 0;
}

int test_refused_at_once() {
    Harness h;
    CHECK(make(h, "test_elastic_cmd_refused"));
    const std::size_t files = files_in(h.rig.audio_dir());
    std::string why;
    h.recording = true;
    CHECK(h.run("polyphonic ratio=2", why) == DispatchResult::Failed);
    CHECK(why.find("stop recording") != std::string::npos);
    h.recording = false;
    h.folder.clear();
    CHECK(h.run("polyphonic ratio=2", why) == DispatchResult::Failed);
    CHECK(why.find("save it first") != std::string::npos);
    h.folder = h.rig.dir;
    CHECK(h.run("polyphonic ratio=9", why) == DispatchResult::Failed);
    CHECK(!h.service->active() && h.said.empty());
    CHECK(files_in(h.rig.audio_dir()) == files);
    return 0;
}

int test_cancel_and_busy() {
    Harness h;
    CHECK(make(h, "test_elastic_cmd_cancel"));
    const std::size_t files = files_in(h.rig.audio_dir());
    const std::size_t undo = h.rig.doc->history().undo_depth();
    std::string why;
    CHECK(h.run("polyphonic ratio=2", why) == DispatchResult::Ok);
    CHECK(h.run("polyphonic ratio=3", why) == DispatchResult::Failed);
    CHECK(why.find("already being stretched") != std::string::npos);
    CHECK(h.service->cancel());
    h.finish();
    CHECK(h.said.back().first == api::RenderState::Cancelled);
    CHECK(files_in(h.rig.audio_dir()) == files && h.rig.doc->history().undo_depth() == undo);
    CHECK(!h.service->cancel());
    return 0;
}

int test_session_closed() {
    Harness h;
    CHECK(make(h, "test_elastic_cmd_closed"));
    const std::size_t files = files_in(h.rig.audio_dir());
    std::string why;
    CHECK(h.run("polyphonic ratio=2", why) == DispatchResult::Ok);
    h.service->abandon();
    CHECK(!h.service->active());
    CHECK(h.said.back().first == api::RenderState::Cancelled);
    CHECK(h.said.back().second.find("closed") != std::string::npos);
    CHECK(files_in(h.rig.audio_dir()) == files);
    return 0;
}

int test_edit_meanwhile() {
    Harness h;
    CHECK(make(h, "test_elastic_cmd_edit"));
    const std::size_t files = files_in(h.rig.audio_dir());
    std::string why;
    CHECK(h.run("polyphonic ratio=2", why) == DispatchResult::Ok);
    model::MoveRegion move;
    move.playlist = h.rig.playlist;
    move.region = h.rig.region;
    move.position = 200000;
    model::ReplicationUpdate u;
    CHECK(h.rig.doc->apply(move, u, why));
    const std::size_t undo = h.rig.doc->history().undo_depth();
    h.release.store(true);
    h.finish();
    CHECK(h.said.back().first == api::RenderState::Failed);
    CHECK(h.said.back().second.find("edited while") != std::string::npos);
    CHECK(files_in(h.rig.audio_dir()) == files && h.rig.doc->history().undo_depth() == undo);
    CHECK(h.changed == 0);
    return 0;
}

int test_take_meanwhile() {
    Harness h;
    CHECK(make(h, "test_elastic_cmd_take"));
    const std::size_t files = files_in(h.rig.audio_dir());
    std::string why;
    CHECK(h.run("polyphonic ratio=2", why) == DispatchResult::Ok);
    h.recording = true;
    h.release.store(true);
    h.finish();
    CHECK(h.said.back().first == api::RenderState::Failed);
    CHECK(h.said.back().second.find("take started") != std::string::npos);
    CHECK(files_in(h.rig.audio_dir()) == files && h.changed == 0);
    return 0;
}

}  // namespace

int main() {
    if (test_absent_without_library() != 0) return 1;
    if (test_apply_and_undo() != 0) return 1;
    if (test_refused_at_once() != 0) return 1;
    if (test_cancel_and_busy() != 0) return 1;
    if (test_session_closed() != 0) return 1;
    if (test_edit_meanwhile() != 0) return 1;
    if (test_take_meanwhile() != 0) return 1;
    std::printf("elastic_commands: ok\n");
    return 0;
}
