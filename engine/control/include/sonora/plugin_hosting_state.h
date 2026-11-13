// SPDX-License-Identifier: GPL-3.0-or-later
// The GetState requests a FlushPluginState has out (ADR-0151, host_control.h
// timeouts): when each is sent and how long its host has to answer. Control
// thread only; never reached from Session::process(). Time is passed in, so
// the deadlines are tested on a fake clock.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "sonora/model/model.h"

namespace sonora {

namespace plugin_client {
class HostProcess;
}

class PluginStateRequests {
public:
    struct Request {
        uint64_t flush = 0;
        model::Id insert = 0;
        model::Id track = 0;
        // The chain's bound host when asked; every event that unbinds it
        // fails the track's requests first, so it is never read after.
        plugin_client::HostProcess* host = nullptr;
        std::string where;
        std::string frame;       // the encoded GetState, until it is sent
        int timeout_ms = 0;      // pc::state_reply_timeout_ms of it
        bool copies = false;     // a save_dir without kGetStateNoCopy
        uint64_t sent = 0;       // 0: not sent yet; else the order sent in
        // False while not sent, or sent behind another on its host.
        bool armed = false;
        uint64_t deadline_ms = 0;
    };

    // `id` is unique across hosts and never reused while outstanding.
    void add(uint32_t id, Request r);

    // The requests to send now, marked sent, in the order to send them:
    // every one that copies nothing, and the oldest copying one while no
    // copying one is out on any host (ADR-0151 §1), so the free-space margin
    // a host checks is the one the last save left. A host answers its
    // GetStates in the order they reach it, so a request's deadline starts
    // when it is the first sent and unanswered on its host. Call again after
    // anything that answers, fails or removes a request.
    // Each comes with its frame, moved out.
    std::vector<std::pair<uint32_t, std::string>> due(uint64_t now_ms);

    // The request answered by `track`'s host, removed; the next one sent on
    // that host starts its deadline. False when `id` is not one `track` has.
    bool answered(uint32_t id, model::Id track, uint64_t now_ms, Request& out);
    // Removes one request (its GetState could not be sent); the next one
    // sent on its host starts its deadline.
    Request remove(uint32_t id, uint64_t now_ms);
    // Removes every request of `track`, sent or waiting (its host stopped,
    // restarted or is replaced), in id order.
    std::vector<Request> remove_track(model::Id track);

    // One request per track past its deadline: the one its host is
    // serving, for its host and its timeout.
    std::vector<Request> late(uint64_t now_ms) const;

    bool flush_pending(uint64_t flush) const;
    bool empty() const { return requests_.empty(); }
    const Request* find(uint32_t id) const;

private:
    void arm_next(model::Id track, uint64_t now_ms);
    bool copy_out() const;

    std::map<uint32_t, Request> requests_;
    uint64_t next_sent_ = 1;
};

}  // namespace sonora
