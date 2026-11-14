// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/editor_service/host_editor_service.h"

#include <cstdio>
#include <vector>

using namespace sonora::editor_service;
namespace pl = sonora::plugin::control;

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

struct FakeLink : EditorHostLink {
    bool host_up = true;
    std::vector<pl::OpenEditor> opens;
    std::vector<pl::CloseEditor> closes;
    bool send_open(TrackIndex, const pl::OpenEditor& m) override {
        if (!host_up) return false;
        opens.push_back(m);
        return true;
    }
    void send_close(TrackIndex, const pl::CloseEditor& m) override { closes.push_back(m); }
};

struct Rig {
    FakeLink link;
    HostEditorService svc{link};
    std::vector<EditorState> seen;
    Rig() { svc.set_listener([this](TrackIndex, SlotIndex, const EditorStatus& s) { seen.push_back(s.state); }); }
    void opened(TrackIndex t, SlotIndex s) { svc.on_editor_opened(t, pl::EditorOpened{link.opens.back().request, s}); }
};
}  // namespace

int main() {
    {
        Rig r;
        CHECK(r.svc.open(1, 2).state == EditorState::Opening);
        CHECK(r.link.opens.size() == 1 && r.link.opens[0].instance == 2);
        CHECK(r.link.opens[0].parent_token.empty());
        r.opened(1, 2);
        CHECK(r.svc.status(1, 2).state == EditorState::Open);
        CHECK(r.svc.open(1, 2).state == EditorState::Open);  // raise
        CHECK(r.link.opens.size() == 1);
        CHECK((r.seen == std::vector<EditorState>{EditorState::Opening, EditorState::Open}));
    }
    {
        Rig r;
        r.svc.open(0, 0);
        r.svc.on_editor_failed(0, pl::EditorFailed{r.link.opens.back().request, 0, "no ui"});
        CHECK(r.svc.status(0, 0).state == EditorState::Failed && r.svc.status(0, 0).reason == "no ui");
        CHECK(r.svc.open(0, 0).state == EditorState::Opening);  // retried
        CHECK(r.link.opens.size() == 2);
    }
    {
        Rig r;
        r.svc.open(0, 0);
        r.opened(0, 0);
        r.svc.open(0, 1);
        r.svc.open(3, 0);
        r.opened(3, 0);
        r.svc.on_host_stopped(0);
        CHECK(r.svc.status(0, 0).state == EditorState::Closed);
        CHECK(r.svc.status(0, 1).state == EditorState::Closed);
        CHECK(r.svc.status(3, 0).state == EditorState::Open);
    }
    {
        Rig r;
        r.svc.open(0, 0);
        r.opened(0, 0);
        r.svc.on_insert_removed(0, 0);
        CHECK(r.svc.status(0, 0).state == EditorState::Closed);
        const auto n = r.seen.size();
        r.svc.close(0, 0);  // closed: ignored
        CHECK(r.link.closes.empty() && r.seen.size() == n);
        r.svc.open(0, 0);
        r.opened(0, 0);
        r.svc.close(0, 0);
        CHECK(r.link.closes.size() == 1 && r.svc.status(0, 0).state == EditorState::Closed);
        r.svc.on_editor_closed(0, pl::EditorClosed{0, pl::EditorCloseReason::Requested, {}});
        CHECK(r.svc.status(0, 0).state == EditorState::Closed);
    }
    {
        Rig r;
        r.link.host_up = false;
        CHECK(r.svc.open(0, 0).state == EditorState::Failed);
        r.svc.open(0, 1);
        r.link.host_up = true;
        r.svc.open(0, 1);
        r.svc.on_editor_closed(0, pl::EditorClosed{1, pl::EditorCloseReason::ByOperator, {}});
        CHECK(r.svc.status(0, 1).state == EditorState::Closed);
    }
    if (failures == 0) std::printf("ok\n");
    return failures == 0 ? 0 : 1;
}
