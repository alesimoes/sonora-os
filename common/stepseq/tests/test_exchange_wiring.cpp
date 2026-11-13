// Editor -> PatternPublisher -> PatternExchange -> PatternFeed -> player
// (ADR-0169): an edit reaches the player on the next block, a refused publish
// goes out on a later tick with the newest editor copy, and the per-block
// acquire path neither allocates nor takes a lock.

#include <atomic>
#include <new>

#include "helpers.h"
#include "sonora/stepseq/pattern_feed.h"
#include "sonora/stepseq/pattern_publisher.h"

static long g_allocs = 0;
void* operator new(std::size_t n) {
    ++g_allocs;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

// A lock-free atomic pointer is what keeps acquire() off any mutex.
static_assert(std::atomic<Pattern*>::is_always_lock_free, "acquire() would lock");

using namespace stepseq_test;

namespace {

struct Rig {
    std::unique_ptr<PatternExchange> exchange = std::make_unique<PatternExchange>();
    StepPatternPlayer player;
    PatternFeed feed{*exchange, player};
    PatternPublisher publisher{*exchange};
    std::unique_ptr<EventBlock> out = std::make_unique<EventBlock>();
    std::int64_t at = 0;

    // One 6000-frame block = one step at the default tempo.
    const EventBlock& block() {
        feed.render_block(at, 6000, tempo(), *out);
        at += 6000;
        return *out;
    }
};

Pattern with_step(int row, std::uint8_t velocity) {
    Pattern p = base_pattern();
    Step s = on_step();
    s.velocity = velocity;
    for (int i = 0; i < p.length(); ++i) p.set_step(row, i, s);
    return p;
}

void edit_reaches_player() {
    Rig r;
    r.player.reset(1);
    CHECK(r.block().count == 0); // the exchange starts with an empty pattern

    Pattern editor = with_step(0, 90);
    CHECK(r.publisher.submit(editor));
    const EventBlock& b = r.block();
    CHECK(b.count == 1);
    CHECK(b.events[0].source == 7 && b.events[0].velocity == 90);

    Row muted = editor.row(0);
    muted.mute = true;
    editor.set_row(0, muted);
    CHECK(r.publisher.submit(editor));
    CHECK(r.block().count == 0);
}

void refused_publish_retries() {
    Rig r;
    r.player.reset(1);
    CHECK(r.publisher.submit(with_step(0, 10)));
    CHECK(r.block().events[0].velocity == 10); // audio holds the first slot
    CHECK(r.publisher.submit(with_step(0, 20)));

    // The spare slot is the one the audio thread still reads: refused, not waited on.
    CHECK(!r.publisher.submit(with_step(0, 30)));
    CHECK(r.publisher.pending() && r.publisher.refusals() == 1);
    CHECK(!r.publisher.retry(with_step(0, 30))); // same tick, still held
    CHECK(r.publisher.pending());

    CHECK(r.block().events[0].velocity == 20); // audio moves to the published slot
    // The editor changed again while pending: the retry sends the newest copy.
    CHECK(r.publisher.retry(with_step(0, 40)));
    CHECK(!r.publisher.pending());
    CHECK(r.block().events[0].velocity == 40);
    CHECK(r.publisher.retry(with_step(0, 50))); // nothing pending: no publish
    CHECK(r.block().events[0].velocity == 40);
}

void acquire_path_does_not_allocate() {
    Rig r;
    r.player.reset(1);
    CHECK(r.publisher.submit(with_step(1, 100)));
    const long before = g_allocs;
    for (int i = 0; i < 64; ++i) {
        r.block();
        if (i == 31) CHECK(r.publisher.submit(with_step(0, 70)));
    }
    CHECK(g_allocs == before);
}

} // namespace

int main() {
    edit_reaches_player();
    refused_publish_retries();
    acquire_path_does_not_allocate();
    std::printf("stepseq exchange wiring: ok\n");
    return 0;
}
