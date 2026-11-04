// SPDX-License-Identifier: GPL-3.0-or-later
// The surface boundary round trip with a fake surface: events in reach the
// engine side in order, feedback comes back, a full ring drops and counts
// and never blocks, and a producer thread loses nothing it was allowed to
// send.
#include <cstdio>
#include <cstring>
#include <thread>

#include "sonora/surfaces/surface_link.h"

using namespace sonora::surfaces;

namespace {
int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

// The engine side as a test sees it: a model of strips, fed by events and
// answering with feedback.
struct FakeEngine {
    float fader[kMaxStrips]{};
    bool mute[kMaxStrips]{};
    bool playing{false};
    void drain(SurfaceLink& l) {
        SurfaceEvent e{};
        while (l.pop_event(e)) {
            switch (e.kind) {
                case EventKind::Fader: fader[e.strip] = e.value; break;
                case EventKind::Mute: mute[e.strip] = e.flag; break;
                case EventKind::Transport:
                    if (e.transport == TransportCmd::Play) playing = true;
                    if (e.transport == TransportCmd::Stop) playing = false;
                    break;
                default: break;
            }
            SurfaceFeedback f{};
            f.kind = FeedbackKind::Fader;
            f.strip = e.strip;
            f.value = fader[e.strip];
            l.push_feedback(f);
        }
    }
};
}  // namespace

int main() {
    {
        SurfaceLink link;
        FakeEngine eng;
        SurfaceEvent e{};
        e.kind = EventKind::Fader; e.strip = 3; e.value = 0.75F;
        CHECK(link.push_event(e));
        e.kind = EventKind::Mute; e.strip = 3; e.flag = true;
        CHECK(link.push_event(e));
        e.kind = EventKind::Transport; e.transport = TransportCmd::Play;
        CHECK(link.push_event(e));
        eng.drain(link);
        CHECK(eng.fader[3] == 0.75F);
        CHECK(eng.mute[3]);
        CHECK(eng.playing);
        SurfaceFeedback f{};
        CHECK(link.pop_feedback(f));
        CHECK(f.kind == FeedbackKind::Fader && f.strip == 3 && f.value == 0.75F);
        int n = 1;
        while (link.pop_feedback(f)) ++n;
        CHECK(n == 3);
    }
    {  // Scribble text pads and truncates.
        SurfaceFeedback f{};
        f.set_text("Vox");
        CHECK(std::memcmp(f.text, "Vox    ", kScribbleChars) == 0);
        f.set_text("LongerThanSeven");
        CHECK(std::memcmp(f.text, "LongerT", kScribbleChars) == 0);
    }
    {  // A full ring drops and counts, in both directions, without waiting.
        SurfaceLink link(4);
        SurfaceEvent e{};
        int accepted = 0;
        for (int i = 0; i < 100; ++i) accepted += link.push_event(e) ? 1 : 0;
        CHECK(accepted >= 1 && accepted <= 4);
        CHECK(link.events_dropped() == static_cast<std::uint64_t>(100 - accepted));
        SurfaceFeedback f{};
        int fa = 0;
        for (int i = 0; i < 100; ++i) fa += link.push_feedback(f) ? 1 : 0;
        CHECK(link.feedback_dropped() == static_cast<std::uint64_t>(100 - fa));
    }
    {  // Surface thread against the engine thread: order kept, drops counted.
        SurfaceLink link(64);
        constexpr int kN = 20000;
        std::thread surface([&] {
            for (int i = 0; i < kN; ++i) {
                SurfaceEvent e{};
                e.value = static_cast<float>(i);
                link.push_event(e);
            }
        });
        float last = -1.0F;
        std::uint64_t got = 0;
        bool ordered = true;
        SurfaceEvent e{};
        for (;;) {
            if (link.pop_event(e)) {
                ordered = ordered && e.value > last;
                last = e.value;
                ++got;
            } else if (got + link.events_dropped() >= static_cast<std::uint64_t>(kN)) {
                break;
            }
        }
        surface.join();
        CHECK(ordered);
        CHECK(got + link.events_dropped() == static_cast<std::uint64_t>(kN));
    }
    std::printf(failures == 0 ? "surface_link: ok\n" : "surface_link: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
