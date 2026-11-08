// SPDX-License-Identifier: GPL-3.0-or-later
//
// The editor service contract (ADR-0088) against its fake:
//   raise        a second open of an open (or opening) editor raises it;
//                no second window, no second state change
//   failed       a failed open carries the host's reason, unchanged, and a
//                later open tries again
//   close        closing a closed or failed editor is ignored
//   host stop    a stopped host or a removed insert reports Closed
#include "sonora/editor_service/plugin_editor_service.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace sonora::editor_service;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

struct Change {
    TrackIndex track;
    SlotIndex slot;
    EditorStatus status;
};

struct Recorder {
    std::vector<Change> changes;
    void attach(PluginEditorService& service) {
        service.set_listener([this](TrackIndex t, SlotIndex s, const EditorStatus& st) {
            changes.push_back(Change{t, s, st});
        });
    }
};

void open_twice_raises() {
    FakePluginEditorService service;
    Recorder rec;
    rec.attach(service);

    check(service.open(1, 2).state == EditorState::Open, "first open is Open");
    check(service.open(1, 2).state == EditorState::Open, "second open stays Open");
    check(service.windows_created() == 1, "one window per insert");
    check(service.raises() == 1, "second open raises");
    check(rec.changes.size() == 1, "raise is not a state change");

    service.hold_opening(3, 0);
    check(service.open(3, 0).state == EditorState::Opening, "held open is Opening");
    check(service.open(3, 0).state == EditorState::Opening, "open while Opening stays Opening");
    service.complete_open(3, 0);
    check(service.status(3, 0).state == EditorState::Open, "completed open is Open");
    check(service.windows_created() == 2, "opening twice still makes one window");
}

void failed_carries_reason() {
    FakePluginEditorService service;
    Recorder rec;
    rec.attach(service);
    const std::string reason = "no display: DISPLAY is not set";

    service.script_failure(0, 1, reason);
    const EditorStatus st = service.open(0, 1);
    check(st.state == EditorState::Failed, "scripted open is Failed");
    check(st.reason == reason, "Failed carries the host's reason");
    check(service.status(0, 1).reason == reason, "status keeps the reason");
    check(rec.changes.size() == 1 && rec.changes[0].status.reason == reason,
          "listener sees the reason");

    check(service.open(0, 1).state == EditorState::Open, "Failed is not sticky");
    check(service.status(0, 1).reason.empty(), "Open has no reason");
}

void close_of_closed_ignored() {
    FakePluginEditorService service;
    Recorder rec;
    rec.attach(service);

    service.close(4, 4);
    check(rec.changes.empty(), "close of a never-opened editor is ignored");
    check(service.status(4, 4).state == EditorState::Closed, "unknown insert is Closed");

    service.open(4, 4);
    service.close(4, 4);
    service.close(4, 4);
    check(rec.changes.size() == 2, "second close is ignored");
    check(service.status(4, 4).state == EditorState::Closed, "closed after close");

    service.script_failure(5, 0, "LV2 UI: not hosted");
    service.open(5, 0);
    service.close(5, 0);
    check(service.status(5, 0).state == EditorState::Failed, "close of Failed is ignored");
}

void host_stop_reports_closed() {
    FakePluginEditorService service;
    Recorder rec;
    rec.attach(service);

    service.open(2, 0);
    service.hold_opening(2, 1);
    service.open(2, 1);
    service.open(7, 0);
    rec.changes.clear();

    service.host_stopped(2);
    check(service.status(2, 0).state == EditorState::Closed, "open editor Closed on host stop");
    check(service.status(2, 1).state == EditorState::Closed, "opening editor Closed on host stop");
    check(service.status(7, 0).state == EditorState::Open, "other host's editor untouched");
    check(rec.changes.size() == 2, "one Closed per editor of the stopped host");
    for (const Change& c : rec.changes)
        check(c.track == 2 && c.status.state == EditorState::Closed, "Closed reported for track 2");

    service.insert_removed(7, 0);
    check(service.status(7, 0).state == EditorState::Closed, "removed insert reports Closed");
}

}  // namespace

int main() {
    open_twice_raises();
    failed_carries_reason();
    close_of_closed_ignored();
    host_stop_reports_closed();
    if (failures == 0) std::printf("plugin_editor_service: all passed\n");
    return failures == 0 ? 0 : 1;
}
