// SPDX-License-Identifier: GPL-3.0-or-later
//
// The control API's conformance suite (ADR-0007 §7): what every
// EngineControl must do, run against FakeEngine in contracts/api's ctest
// and against InProcessEngine on drivers/fake in engine/control's, so the
// fake cannot drift from the real engine.
//
// A Rig makes engines and lets time pass. The suite drives them only
// through EngineControl and StateView, as an interface would.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "sonora/api/engine_control.h"

namespace sonora::api::conformance {

class Rig {
public:
    virtual ~Rig() = default;
    virtual const char* name() const = 0;
    // A fresh engine, not yet connected, whose Hello announces these
    // versions. Each call is a new engine with a new session.
    virtual std::unique_ptr<EngineControl> make(uint32_t protocol_version,
                                                uint32_t state_layout_version) = 0;
    // The clock StateView::engine_alive() is judged on.
    virtual uint64_t now_ns() = 0;
    // Lets `ns` of that clock pass: a sleep for a real engine, a step of
    // the injected clock for the fake.
    virtual void wait_ns(uint64_t ns) = 0;
    // The engine stops as a crashed process would: nothing more is
    // published, and nothing answers.
    virtual void kill(EngineControl& engine) = 0;
    // Whether the inputs carry a signal, so a published peak is above zero.
    virtual bool has_signal() const = 0;
    // From the next take on, writing fails as at a full disk (true), or
    // records cleanly (false). Track 1 is the one the suite arms. False if
    // this rig cannot do it; the check is then skipped, and says so.
    virtual bool set_takes_fail(EngineControl& engine, bool fail) {
        (void)engine;
        (void)fail;
        return false;
    }
    // A take closed while rolling (a punch-out) stays closing, its files not
    // yet in the session, until released (false). Never held across a Stop.
    // False if this rig cannot do it (ADR-0023 §5).
    virtual bool hold_takes_closing(EngineControl& engine, bool hold) {
        (void)engine;
        (void)hold;
        return false;
    }
    // A Record waits to start, as it waits for its window, until released.
    // False if this rig cannot do it.
    virtual bool hold_record_start(EngineControl& engine, bool hold) {
        (void)engine;
        (void)hold;
        return false;
    }
    // Every capture input goes away (false) or comes back (true), as when
    // an interface is unplugged and plugged in again. False if this rig
    // cannot do it.
    virtual bool set_inputs_present(EngineControl& engine, bool present) {
        (void)engine;
        (void)present;
        return false;
    }
    // The operator moves `key` of a running insert to `value` in the
    // plugin's own editor, then ends the gesture (`close` false) or closes
    // the editor without one (`close` true): the host's ParamTouched and
    // EditorClosed. False if this rig cannot do it.
    virtual bool edit_in_plugin_editor(EngineControl& engine, uint64_t insert,
                                       const std::string& key, float value, bool close) {
        (void)engine;
        (void)insert;
        (void)key;
        (void)value;
        (void)close;
        return false;
    }
    // A running insert's plugin now has this state: what its GetState would
    // be written as. False if this rig cannot do it.
    virtual bool set_plugin_state(EngineControl& engine, uint64_t insert,
                                  const std::string& sha256, uint64_t bytes) {
        (void)engine;
        (void)insert;
        (void)sha256;
        (void)bytes;
        return false;
    }
    // A take file the session does not name, as a crash leaves one:
    // `name` in the session's "Audio Files/", mono, `frames` long, with
    // the header the engine writes for take `take` on the track whose id is
    // `track_id`, of a session called `session` whose id is `session_id`
    // (empty: a take from before session ids), at TimeReference
    // `position`. The engine finds it at the next session open. False if
    // this rig cannot do it.
    virtual bool leave_orphan_take(EngineControl& engine, const std::string& name,
                                   const std::string& session, const std::string& session_id,
                                   int take, uint64_t track_id, uint64_t position,
                                   uint64_t frames) {
        (void)engine;
        (void)name;
        (void)session;
        (void)session_id;
        (void)take;
        (void)track_id;
        (void)position;
        (void)frames;
        return false;
    }
    // A cable from output `output` to input `input`, port numbers from 1,
    // that every engine this rig makes has, and its round trip in frames
    // (ADR-0019, update of 2026-10-26). False if this rig has none; the
    // latency check then measures nothing on a cable, and says so.
    virtual bool latency_cable(uint32_t& output, uint32_t& input, uint32_t& frames) const {
        (void)output;
        (void)input;
        (void)frames;
        return false;
    }
    // Whether the engine puts a closed take on its tracks' active playlists
    // (engine/control does; the fake leaves the document alone).
    virtual bool places_closed_takes() const { return false; }
    // How many files of take `take` the engine wrote into `folder`'s
    // "Audio Files/": a real engine's on disk, the fake's as it remembers
    // them. -1 if this rig cannot tell; the check then says so.
    virtual int take_files_in(EngineControl& engine, const std::string& folder, int take) {
        (void)engine;
        (void)folder;
        (void)take;
        return -1;
    }
};

// Runs every check, prints "ok <check>" or "FAIL <check>: <why>" for each,
// and returns the number that failed.
int run(Rig& rig);

}  // namespace sonora::api::conformance
