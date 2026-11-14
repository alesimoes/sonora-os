// SPDX-License-Identifier: GPL-3.0-or-later
//
// audio.elastic's edit (ADR-0136 §2, §5) with the fake stretcher: the take is
// only read, the copy holds the stretched span and its handles, the region
// points at it in one undo step, a result that would reach the next region
// is refused before anything is written, and a cancelled render leaves no
// file behind.
#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "elastic_rig.h"
#include "sonora/elastic/elastic_job.h"
#include "sonora/elastic/elastic_plan.h"
#include "sonora/wav_reader.h"

namespace {

using namespace sonora;
using namespace elastic_test;

elastic::StretcherFactory linear() {
    return [](std::uint32_t, const std::atomic<bool>&) {
        return std::unique_ptr<stretch::Stretcher>(new stretch::LinearResampleStretcher());
    };
}

// Fixed onsets, region-relative, for Quantize Audio.
class FixedOnsets final : public elastic::OnsetSource {
public:
    explicit FixedOnsets(std::vector<std::int64_t> at) : at_(std::move(at)) {}
    bool find(const stretch::Planar&, std::int64_t, std::int64_t, std::uint32_t,
              std::vector<std::int64_t>& out, std::string&) override {
        out = at_;
        return true;
    }

private:
    std::vector<std::int64_t> at_;
};

// Stretches, then cancels the render it is part of, as an operator's Cancel
// that lands while the copy is being written.
class CancellingStretcher final : public stretch::Stretcher {
public:
    explicit CancellingStretcher(std::atomic<bool>& cancel) : cancel_(cancel) {}
    bool process(const stretch::Planar& in, const stretch::WarpMap& map, stretch::Algorithm a,
                 stretch::Planar& out, std::string& why) override {
        const bool ok = stretch::LinearResampleStretcher().process(in, map, a, out, why);
        cancel_.store(true);
        return ok;
    }

private:
    std::atomic<bool>& cancel_;
};

bool plan_for(const Rig& r, const std::string& arg, elastic::ElasticPlan& plan, std::string& why) {
    api::ElasticRequest rq;
    if (!api::parse_elastic_request(arg, rq, why)) return false;
    return elastic::elastic_check(r.doc->current(), rq, plan, why);
}

bool apply_elastic(Rig& r, const std::string& arg, std::string& why,
                   elastic::OnsetSource* onsets = nullptr) {
    elastic::ElasticPlan plan;
    if (!plan_for(r, arg, plan, why)) return false;
    std::atomic<bool> cancel{false};
    std::atomic<std::uint32_t> permille{0};
    elastic::ElasticOutput out;
    if (!elastic::render_elastic(plan, r.dir, linear(), onsets, cancel, permille, out, why)) {
        return false;
    }
    model::Compound c;
    if (!elastic::elastic_compound(r.doc->current(), plan, out, c, why)) return false;
    model::ReplicationUpdate u;
    return r.doc->apply(c, u, why);
}

bool read_frame(const std::string& path, std::int64_t i, float& out) {
    WavReader rd;
    std::string e;
    if (!rd.open(path, e)) return false;
    std::vector<float> v(rd.channels());
    if (rd.read(i, 1, v.data()) != 1) return false;
    out = v[0];
    return true;
}

std::string arg(const Rig& r, const std::string& rest) {
    return std::to_string(r.region) + " " + rest;
}

int test_ratio_is_one_undo_step() {
    Rig r;
    // 6 s take; a 0.5 s region at 1 s with 2 s of handle either side.
    CHECK(make_rig(r, "test_elastic_ratio", 288000, 48000, 24000, 120000));
    const std::string before = slurp(r.take());
    const std::size_t undo = r.doc->history().undo_depth();
    std::string why;
    CHECK(apply_elastic(r, arg(r, "polyphonic ratio=2"), why));
    CHECK(r.doc->history().undo_depth() == undo + 1);
    CHECK(slurp(r.take()) == before);

    const model::Region* g = region_of(r, r.region);
    CHECK(g != nullptr && g->position == 48000 && g->length == 48000);
    CHECK(g->source_offset == 96000 && g->channels.size() == 2);
    CHECK(g->channels[0].source == g->channels[1].source && g->channels[1].channel == 1);
    const model::Source* made = source_of(r, r.region);
    CHECK(made != nullptr && made->id != r.source);
    CHECK(made->path == "Audio Files/Drums_elastic_01.wav");
    CHECK(made->length == 96000 + 48000 + 96000 && made->channels == 2);
    // The handle keeps its speed; the region plays at half speed.
    const std::string copy = r.dir + "/" + made->path;
    float v = 9.0f;
    CHECK(read_frame(copy, 0, v) && v == take_at(24000));
    for (std::int64_t k = 0; k < 24000; k += 997) {
        CHECK(read_frame(copy, 96000 + 2 * k, v) && std::fabs(v - take_at(120000 + k)) < 1e-4f);
    }

    model::ReplicationUpdate u;
    CHECK(r.doc->undo(u, why));
    g = region_of(r, r.region);
    CHECK(g->channels[0].source == r.source && g->length == 24000 && g->source_offset == 120000);
    CHECK(slurp(r.take()) == before);

    // A second render numbers its file after the first.
    CHECK(apply_elastic(r, arg(r, "rhythmic ratio=0.5"), why));
    CHECK(region_of(r, r.region)->length == 12000);
    CHECK(source_of(r, r.region)->path == "Audio Files/Drums_elastic_02.wav");
    CHECK(slurp(r.take()) == before);
    return 0;
}

int test_overlap_refused() {
    Rig r;
    CHECK(make_rig(r, "test_elastic_overlap", 288000, 48000, 24000, 120000));
    model::Id next = 0;
    CHECK(place_region(r, 48000 + 25000, 10000, 0, next));
    const std::size_t files = files_in(r.audio_dir());
    const std::size_t undo = r.doc->history().undo_depth();
    std::string why;
    CHECK(!apply_elastic(r, arg(r, "polyphonic ratio=2"), why));
    CHECK(why.find("into the next region") != std::string::npos);
    CHECK(!apply_elastic(r, arg(r, "polyphonic warp=12000:14000"), why));
    CHECK(files_in(r.audio_dir()) == files && r.doc->history().undo_depth() == undo);
    // Up to the next region's first frame fits.
    CHECK(apply_elastic(r, arg(r, "polyphonic warp=12000:13000,20000:21000"), why));
    CHECK(region_of(r, r.region)->length == 25000);
    CHECK(region_of(r, next)->position == 73000 && region_of(r, next)->length == 10000);
    return 0;
}

int test_warp_and_gain_line() {
    Rig r;
    CHECK(make_rig(r, "test_elastic_warp", 288000, 48000, 24000, 120000, 120000 + 18000));
    std::string why;
    CHECK(apply_elastic(r, arg(r, "monophonic warp=6000:12000,18000:20000"), why));
    const model::Region* g = region_of(r, r.region);
    CHECK(g->length == 20000 + 6000);
    // The point under source frame 18000 of the region lands on 20000.
    CHECK(g->gain_line.size() == 1 && g->gain_line[0].at == 96000 + 20000);
    return 0;
}

int test_region_at_end_of_take() {
    Rig r;
    // No handle after the region: the take ends with it.
    CHECK(make_rig(r, "test_elastic_end", 96000, 0, 24000, 72000));
    std::string why;
    CHECK(apply_elastic(r, arg(r, "varispeed ratio=1.5"), why));
    const model::Region* g = region_of(r, r.region);
    CHECK(g->length == 36000 && g->source_offset == 72000);
    CHECK(source_of(r, r.region)->length == 72000 + 36000);
    return 0;
}

int test_quantize() {
    Rig r;
    // 120 bpm at 48 kHz: a sixteenth is 6000 frames, the region starts on a beat.
    CHECK(make_rig(r, "test_elastic_quantize", 288000, 48000, 24000, 120000));
    std::string why;
    FixedOnsets onsets({6100, 12500});
    CHECK(apply_elastic(r, arg(r, "rhythmic quantize=0.25"), why, &onsets));
    CHECK(region_of(r, r.region)->length == 12000 + (24000 - 12500));

    const std::size_t files = files_in(r.audio_dir());
    const std::size_t undo = r.doc->history().undo_depth();
    FixedOnsets none({});
    CHECK(!apply_elastic(r, arg(r, "rhythmic quantize=0.25"), why, &none));
    CHECK(why.find("nothing to move") != std::string::npos);
    CHECK(!apply_elastic(r, arg(r, "rhythmic quantize=0.25"), why, nullptr));
    CHECK(files_in(r.audio_dir()) == files && r.doc->history().undo_depth() == undo);
    return 0;
}

int test_cancel_removes_the_copy() {
    Rig r;
    CHECK(make_rig(r, "test_elastic_cancel", 288000, 48000, 24000, 120000));
    const std::size_t files = files_in(r.audio_dir());
    elastic::ElasticPlan plan;
    std::string why;
    CHECK(plan_for(r, arg(r, "polyphonic ratio=2"), plan, why));
    std::atomic<std::uint32_t> permille{0};
    elastic::ElasticOutput out;
    // Cancelled while the copy is written: the file it opened goes.
    std::atomic<bool> cancel{false};
    const elastic::StretcherFactory cancelling = [&cancel](std::uint32_t,
                                                           const std::atomic<bool>&) {
        return std::unique_ptr<stretch::Stretcher>(new CancellingStretcher(cancel));
    };
    CHECK(!elastic::render_elastic(plan, r.dir, cancelling, nullptr, cancel, permille, out, why));
    CHECK(why == "the stretch was cancelled");
    CHECK(files_in(r.audio_dir()) == files);
    // Cancelled before it read anything.
    std::atomic<bool> early{true};
    CHECK(!elastic::render_elastic(plan, r.dir, linear(), nullptr, early, permille, out, why));
    CHECK(files_in(r.audio_dir()) == files);
    // A refusal of the stretcher's leaves nothing either.
    const elastic::StretcherFactory none = [](std::uint32_t, const std::atomic<bool>&) {
        return std::unique_ptr<stretch::Stretcher>();
    };
    std::atomic<bool> go{false};
    CHECK(!elastic::render_elastic(plan, r.dir, none, nullptr, go, permille, out, why));
    CHECK(files_in(r.audio_dir()) == files);
    return 0;
}

int test_refusals() {
    Rig r;
    CHECK(make_rig(r, "test_elastic_refusals", 288000, 48000, 24000, 120000));
    elastic::ElasticPlan plan;
    std::string why;
    CHECK(!plan_for(r, "9999 polyphonic ratio=2", plan, why));
    CHECK(why.find("not in the session") != std::string::npos);
    CHECK(!plan_for(r, arg(r, "polyphonic warp=30000:31000"), plan, why));
    CHECK(why.find("past the region's end") != std::string::npos);
    return 0;
}

}  // namespace

int main() {
    if (test_ratio_is_one_undo_step() != 0) return 1;
    if (test_overlap_refused() != 0) return 1;
    if (test_warp_and_gain_line() != 0) return 1;
    if (test_region_at_end_of_take() != 0) return 1;
    if (test_quantize() != 0) return 1;
    if (test_cancel_removes_the_copy() != 0) return 1;
    if (test_refusals() != 0) return 1;
    std::printf("elastic_edit: ok\n");
    return 0;
}
