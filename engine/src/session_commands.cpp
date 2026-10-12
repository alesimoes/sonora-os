// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/session_commands.h"

#include <cstddef>
#include <string>

#include "sonora/session.h"

namespace sonora {

void install_session_commands(CommandRegistry& reg, Session& session, int& take,
                              bool& quit) {
    // The counter is a hint, never trusted on its own: the folder may hold
    // takes from before a restart, and start_recording() refuses a name that
    // is taken, so the number is re-read from disk before every take.
    const auto start = [&session, &take](std::string& why) {
        const int free = session.next_free_take();
        if (take < free) take = free;
        return session.start_recording(take, &why);
    };

    reg.install("transport.toggle_record",
                [&session, &take, start](const CommandContext&, std::string& why) {
        if (session.transport().state() == TransportState::Recording) {
            session.stop();
            session.disk().start();  // ready for the next take
            ++take;
            return true;
        }
        return start(why);
    });

    // Start, never stop: Pro Tools' Command+Space and F12, Logic's R. Pressed
    // during a take it leaves the take alone rather than closing it and
    // opening the next, which is what a second press of a record key means
    // in both applications.
    reg.install("transport.record",
                [&session, start](const CommandContext&, std::string& why) {
        if (session.transport().state() == TransportState::Recording) {
            return true;
        }
        return start(why);
    });

    reg.install("transport.stop", [&](const CommandContext&) {
        if (session.transport().state() == TransportState::Stopped) {
            return true;
        }
        session.stop();
        session.disk().start();
        ++take;
        return true;
    });

    reg.install("track.toggle_arm", [&](const CommandContext& ctx) {
        // No selection exists yet, and guessing a track is the
        // port-ordering defect in another form.
        if (!ctx.has_target()) return false;
        const std::size_t idx = static_cast<std::size_t>(ctx.target);
        if (idx >= session.tracks().size()) return false;
        auto& tr = session.tracks()[idx];
        tr->set_rec_armed(!tr->rec_armed());
        return true;
    });

    reg.install("track.arm_all", [&](const CommandContext&) {
        for (const auto& tr : session.tracks()) tr->set_rec_armed(true);
        return true;
    });

    reg.install("track.disarm_all", [&](const CommandContext&) {
        for (const auto& tr : session.tracks()) tr->set_rec_armed(false);
        return true;
    });

    reg.install("app.quit", [&](const CommandContext&) {
        quit = true;
        return true;
    });
}

}  // namespace sonora
