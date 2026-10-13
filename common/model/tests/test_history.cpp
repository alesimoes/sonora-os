// SPDX-License-Identifier: GPL-3.0-or-later
//
// Undo as a pointer: round trips, the redo branch, the byte budget, and a
// replica that follows the engine's floor.
#include <variant>

#include "model_test.h"

using namespace model_test;

namespace {

struct Session {
    Id track = 0, playlist = 0, source = 0;
};

int setup(History& h, Session& s, Frames source_length = 100'000'000) {
    Ids ids(h.current());
    const AddTrack t = add_track(ids, "Vox");
    s.track = t.track;
    s.playlist = t.playlist;
    CHECK_APPLY(h, t);
    const AddSource src = add_source(ids, source_length);
    s.source = src.source.id;
    CHECK_APPLY(h, src);
    return 0;
}

Operation place_at(const History& h, const Session& s, Frames position) {
    return PlaceRegion{s.playlist,
                       region(h.current().next_id, s.source, position, 1000, position)};
}

int test_round_trip() {
    History h(new_session(settings()));
    Session s;
    CHECK(setup(h, s) == 0);
    std::vector<std::shared_ptr<const EditState>> seen{h.current().edit};
    for (int i = 0; i < 20; ++i) {
        CHECK_APPLY(h, place_at(h, s, i * 2000));
        seen.push_back(h.current().edit);
    }
    const std::uint64_t rev = h.current().revision;
    // Undo walks back through exactly the states it went through.
    for (int i = 20; i > 0; --i) {
        CHECK(h.current().edit == seen[static_cast<std::size_t>(i)]);
        CHECK(h.undo());
    }
    CHECK(h.current().edit == seen[0]);
    CHECK(h.undo());   // and past them to before the track existed
    CHECK(h.current().edit->tracks.empty());
    CHECK(!h.undo());
    // The pool is outside undo.
    CHECK(h.current().sources->find(s.source) != nullptr);
    CHECK(h.redo());
    for (int i = 1; i <= 20; ++i) {
        CHECK(h.redo());
        CHECK(h.current().edit == seen[static_cast<std::size_t>(i)]);
    }
    CHECK(!h.redo());
    // Every undo and redo is a revision of its own.
    CHECK(h.current().revision == rev + 21 + 21);
    return 0;
}

int test_new_edit_discards_redo() {
    History h(new_session(settings()));
    Session s;
    CHECK(setup(h, s) == 0);
    CHECK_APPLY(h, place_at(h, s, 0));
    CHECK_APPLY(h, place_at(h, s, 5000));
    CHECK(h.undo());
    CHECK(h.can_redo());
    CHECK_APPLY(h, place_at(h, s, 9000));
    CHECK(!h.can_redo());
    CHECK(playlist_of(h.current(), 0).regions.size() == 2);
    // Non-undoable operations neither push a step nor discard redo.
    CHECK(h.undo());
    CHECK(h.can_redo());
    TrackMix m = *h.current().mix.find(s.track);
    m.mute = true;
    CHECK_APPLY(h, SetMix{m});
    CHECK(h.can_redo());
    return 0;
}

// The charge is what a step alone allocated: an edit on one track does not
// pay for another track's long playlist.
int test_charge_is_exclusive() {
    History h(new_session(settings()));
    Session a;
    CHECK(setup(h, a) == 0);
    Ids ids(h.current());
    const AddTrack big = add_track(ids, "Big");
    CHECK_APPLY(h, big);
    for (int i = 0; i < 2000; ++i) {
        CHECK_APPLY(h, (PlaceRegion{big.playlist,
                                    region(h.current().next_id, a.source, i * 1000, 1000)}));
    }
    const Snapshot before = h.current();
    CHECK_APPLY(h, place_at(h, a, 0));
    const std::size_t small = exclusive_bytes(*h.current().edit, *before.edit);
    Snapshot after_big;
    std::string err;
    CHECK(apply(before, MoveRegion{big.playlist, before.next_id - 1, 5'000'000, 0},
                after_big, err));
    const std::size_t large = exclusive_bytes(*after_big.edit, *before.edit);
    CHECK(small < 2000);
    CHECK(large > 2000 * sizeof(Region));
    return 0;
}

int test_byte_budget_evicts_oldest() {
    // Enough for a handful of single-region steps on a short playlist.
    History h(new_session(settings()), 4096);
    Session s;
    CHECK(setup(h, s) == 0);
    std::uint64_t last_floor = h.floor();
    bool evicted = false;
    for (int i = 0; i < 200; ++i) {
        CHECK_APPLY(h, place_at(h, s, i * 2000));
        CHECK(h.charged_bytes() <= h.budget() || h.step_count() == 2);
        CHECK(h.floor() >= last_floor);
        evicted = evicted || h.floor() > last_floor;
        last_floor = h.floor();
    }
    CHECK(evicted);
    CHECK(h.step_count() < 200);
    // Undo reaches exactly the floor and no further.
    std::size_t undos = 0;
    while (h.undo()) ++undos;
    CHECK(undos == h.step_count() - 1);
    CHECK(h.current().revision > h.floor());
    // The floor's state is the one recorded at that revision: 200 - undos
    // regions are on the playlist.
    CHECK(playlist_of(h.current(), 0).regions.size() == 200 - undos);
    return 0;
}

int test_newest_step_is_never_evicted() {
    History h(new_session(settings()), 1);
    Session s;
    CHECK(setup(h, s) == 0);
    for (int i = 0; i < 10; ++i) {
        CHECK_APPLY(h, place_at(h, s, i * 2000));
        // Over budget at every step, yet the last edit can still be undone.
        CHECK(h.step_count() == 2);
        CHECK(h.can_undo());
    }
    CHECK(h.undo());
    CHECK(playlist_of(h.current(), 0).regions.size() == 9);
    CHECK(!h.undo());
    return 0;
}

// The engine decides eviction; the replica follows the floor carried by
// each message and so offers the same undo steps, whatever it would have
// counted itself.
int test_replica_follows_floor() {
    History engine(new_session(settings()), 3000);
    History replica = History::replica(engine.current());
    struct Message {
        enum Kind { Op, Undo, Redo } kind;
        Operation op;
        std::uint64_t revision;
        std::uint64_t floor;
    };
    std::vector<Message> log;
    const auto send_op = [&](const Operation& op) {
        std::string err;
        if (!engine.apply(op, err)) return false;
        log.push_back({Message::Op, op, engine.current().revision, engine.floor()});
        return true;
    };
    Ids ids(engine.current());
    const AddTrack t = add_track(ids, "Vox");
    CHECK(send_op(t));
    const AddSource src = add_source(ids, 100'000'000);
    CHECK(send_op(src));
    Session s{t.track, t.playlist, src.source.id};
    for (int i = 0; i < 60; ++i) {
        CHECK(send_op(place_at(engine, s, i * 2000)));
        if (i % 7 == 3) {
            CHECK(engine.undo());
            log.push_back({Message::Undo, {}, engine.current().revision, engine.floor()});
        }
        // The budget keeps few steps, so a second undo may have nothing left.
        if (i % 11 == 5 && engine.can_undo()) {
            CHECK(engine.undo());
            log.push_back({Message::Undo, {}, engine.current().revision, engine.floor()});
            CHECK(engine.redo());
            log.push_back({Message::Redo, {}, engine.current().revision, engine.floor()});
        }
    }
    for (const Message& m : log) {
        if (m.kind == Message::Op) {
            std::string err;
            CHECK(replica.apply(m.op, err));
        } else if (m.kind == Message::Undo) {
            CHECK(replica.undo());
        } else {
            CHECK(replica.redo());
        }
        // A replica checks each revision is the next one.
        CHECK(replica.current().revision == m.revision);
        replica.evict_below(m.floor);
    }
    CHECK(replica.current() == engine.current());
    CHECK(replica.floor() == engine.floor());
    CHECK(replica.undo_depth() == engine.undo_depth());
    CHECK(replica.step_count() == engine.step_count());
    while (engine.undo()) {
        CHECK(replica.undo());
        CHECK(replica.current() == engine.current());
    }
    CHECK(!replica.undo());
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    int run = 0;
    const auto t = [&](int (*f)()) {
        ++run;
        failed += f();
    };
    t(test_round_trip);
    t(test_new_edit_discards_redo);
    t(test_charge_is_exclusive);
    t(test_byte_budget_evicts_oldest);
    t(test_newest_step_is_never_evicted);
    t(test_replica_follows_floor);
    if (failed != 0) {
        std::fprintf(stderr, "model_history: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("model_history: %d passed\n", run);
    return 0;
}
