// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/session_commands.h"

#include <cstddef>
#include <string>

#include "sonora/playback_controller.h"
#include "sonora/session.h"

namespace sonora {

void install_session_commands(CommandRegistry& reg, Session& session, int& take,
                              bool& quit, MayRecord may_record,
                              PlaybackController* playback, PunchFor punch_for,
                              MayArm may_arm, LoopFor loop_for) {
    const auto loop_take = [loop_for](LoopRange& loop, std::string& refusal) {
        refusal.clear();
        return loop_for && loop_for(loop, refusal);
    };
    // The counter is a hint, never trusted on its own: the folder may hold
    // takes from before a restart, and start_recording() refuses a name that
    // is taken, so the number is re-read from disk before every take.
    const auto open_take = [&session, &take, may_record, punch_for,
                            loop_take](std::string& why) {
        if (may_record && !may_record(why)) return false;
        const int free = session.next_free_take();
        if (take < free) take = free;
        // Asked again here: the ranges are the ones set when the take
        // starts, which behind the gate is later than the press.
        LoopRange loop;
        std::string refusal;
        if (loop_take(loop, refusal)) return session.start_loop(take, loop, &why);
        if (!refusal.empty()) {
            why = refusal;
            return false;
        }
        PunchRange range;
        if (punch_for && punch_for(range)) return session.start_punch(take, range, &why);
        return session.start_recording(take, &why);
    };
    // With a playback controller a take starts behind its residency gate,
    // like a roll: what plays under it must be in memory, and a locate asked
    // just before must have landed, or the take starts somewhere else
    // (ADR-0009 §4.4). The request is answered from pump(); a refusal then
    // comes back through take_record_error().
    const auto start = [open_take, playback, loop_take](std::string& why) {
        LoopRange loop;
        std::string refusal;
        const bool looped = loop_take(loop, refusal);
        // ADR-0016 C3: refused at the press, before anything waits.
        if (!refusal.empty()) {
            why = refusal;
            return false;
        }
        if (!playback) return open_take(why);
        // Asked once already: a second press is the same request.
        if (!playback->record_requested()) {
            // A loop take records from its pre-roll (ADR-0016 §1): the gate
            // then waits for that window too, and start_loop() lands there.
            if (looped && !playback->locate(loop.start, why)) return false;
            playback->record(open_take);
        }
        return true;
    };

    // The transport stops whatever the take's fate. Failed reports the take,
    // not the stop: a file that hit the 4 GB limit or a full disk must reach
    // the operator, and this is the moment the session learns of it.
    const auto finish = [&session, &take](std::string& why) {
        const bool ok = session.stop(&why);
        session.disk().start();  // ready for the next take
        ++take;
        return ok;
    };

    reg.install("transport.toggle_record",
                [&session, start, finish, playback](const CommandContext&, std::string& why) {
        if (session.transport().state() == TransportState::Recording) {
            return finish(why);
        }
        // Still waiting for its window: the second press takes it back.
        if (playback && playback->record_requested()) {
            playback->cancel_record();
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

    // Logic's Record/Record Toggle. During a take it punches out with the
    // transport still rolling, the take closed in band (ADR-0009).
    // Otherwise it starts a take as transport.record does: on the fly at
    // the playhead with no punch range, and with one, only inside [A, B),
    // which is what the lit PUNCH indicator promises (Logic's Autopunch).
    // Before A it waits for A, inside the range it punches in at the
    // playhead and out at B, and at or past B start_punch() refuses it.
    reg.install("transport.punch", [&session, &take, start, finish,
                                    playback](const CommandContext&, std::string& why) {
        if (session.transport().state() == TransportState::Recording) {
            // Rolling on out of a loop take would leave the loop with the
            // playhead past B; stop ends a loop take (ADR-0016 §1).
            if (session.loop_take_running()) return finish(why);
            session.punch_out();
            ++take;
            return true;
        }
        if (playback && playback->record_requested()) {
            playback->cancel_record();
            return true;
        }
        return start(why);
    });

    // A roll is not a take: stopping it closes no file and uses no take
    // number.
    reg.install("transport.stop",
                [&session, finish, playback](const CommandContext&, std::string& why) {
        const TransportState state = session.transport().state();
        if (state == TransportState::Recording) return finish(why);
        if (playback) {
            playback->stop();
        } else {
            session.stop_playback();
        }
        return true;
    });

    if (playback) {
        // Rolls once the window at the playhead is resident (ADR-0009 §4.4).
        reg.install("transport.play", [&session, playback](const CommandContext&) {
            if (session.transport().state() == TransportState::Stopped) playback->play();
            return true;
        });
        // Space in Pro Tools: plays, or stops whatever is running, a take
        // included.
        reg.install("transport.toggle_play",
                    [&session, finish, playback](const CommandContext&, std::string& why) {
            const TransportState state = session.transport().state();
            if (state == TransportState::Recording) return finish(why);
            if (state == TransportState::Rolling || playback->play_requested() ||
                playback->record_requested()) {
                playback->stop();
            } else {
                playback->play();
            }
            return true;
        });
        reg.install("transport.return_to_zero",
                    [playback](const CommandContext&, std::string& why) {
            return playback->locate(0, why);
        });
    }

    // Arming is fixed for the length of a take. process() writes a track
    // only while it is armed but leaves its file open, so a track disarmed
    // and re-armed mid-take resumes with no gap and drifts out of sync with
    // the others; a track armed mid-take has no file, and its ring fills and
    // counts overruns. Refusing here keeps the audio thread as it is.
    const auto take_running = [&session](std::string& why) {
        if (session.transport().state() != TransportState::Recording) {
            return false;
        }
        why = "arming cannot change during a take; stop first";
        return true;
    };

    reg.install("track.toggle_arm",
                [&session, take_running, may_arm](const CommandContext& ctx,
                                                  std::string& why) {
        // No selection exists yet, and guessing a track is the
        // port-ordering defect in another form.
        if (!ctx.has_target()) {
            why = "no track given, and there is no selection yet";
            return false;
        }
        const std::size_t idx = static_cast<std::size_t>(ctx.target);
        if (idx >= session.tracks().size()) {
            why = "there is no track " + std::to_string(idx + 1);
            return false;
        }
        if (take_running(why)) return false;
        auto& tr = session.tracks()[idx];
        if (!tr->rec_armed() && may_arm && !may_arm(idx, why)) return false;
        tr->set_rec_armed(!tr->rec_armed());
        return true;
    });

    reg.install("track.arm_all",
                [&session, take_running, may_arm](const CommandContext&, std::string& why) {
        if (take_running(why)) return false;
        // Every track that may record; the others stay as they are.
        std::size_t skipped = 0;
        std::string first_refusal;
        for (std::size_t i = 0; i < session.tracks().size(); ++i) {
            std::string refused;
            if (may_arm && !may_arm(i, refused)) {
                if (skipped++ == 0) first_refusal = refused;
                continue;
            }
            session.tracks()[i]->set_rec_armed(true);
        }
        // An Ok that armed nothing (a session of MIDI tracks) reads as a key
        // that did nothing.
        if (skipped != 0 && skipped == session.tracks().size()) {
            why = "no track can be armed: " + first_refusal;
            return false;
        }
        return true;
    });

    reg.install("track.disarm_all",
                [&session, take_running](const CommandContext&, std::string& why) {
        if (take_running(why)) return false;
        for (const auto& tr : session.tracks()) tr->set_rec_armed(false);
        return true;
    });

    reg.install("app.quit", [&](const CommandContext&) {
        quit = true;
        return true;
    });
}

}  // namespace sonora
