// SPDX-License-Identifier: GPL-3.0-or-later
//
// StemSplitModel: a split imports N tracks and leaves the source clip as it
// was; a cancel or a failure adds none; refusals are held; a new session
// forgets the last one.
#include <cstdio>

#include "sonora/ui_stems/stem_split_model.h"

using namespace sonora;
using namespace sonora::ui_stems;

#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
            return 1;                                                  \
        }                                                              \
    } while (0)

namespace {

struct Tracks final : TrackImporter {
    std::vector<stems::RenderedStem> added;
    bool fail = false;
    bool import_stems(const std::vector<stems::RenderedStem>& s, std::string* err) override {
        if (fail) {
            *err = "no room";
            return false;
        }
        added.insert(added.end(), s.begin(), s.end());
        return true;
    }
};

SourceAudio source(int channels) {
    SourceAudio s;
    s.sample_rate = 48000;
    s.name = "Mix";
    for (int c = 0; c < channels; ++c) {
        std::vector<float> v(4800);
        for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<float>((i + c) % 7) * 0.1f;
        s.channels.push_back(v);
    }
    return s;
}

// FakeSeparator that raises the cancel flag from inside the job.
class CancelOnSeparate final : public stems::StemSeparator {
public:
    explicit CancelOnSeparate(StemSplitModel*& m) : m_(m) {}
    const char* name() const override { return "cancel"; }
    bool supports(stems::StemKind k) const override { return inner_.supports(k); }
    bool separate(const stems::StemRequest& r, stems::StemResult& o, stems::ProgressSink* p,
                  std::string* e) override {
        m_->cancel();
        return inner_.separate(r, o, p, e);
    }

private:
    stems::FakeSeparator inner_;
    StemSplitModel*& m_;
};

SeparatorFactory fake_factory() {
    return [](Method, int) { return std::make_unique<stems::FakeSeparator>(); };
}

int test_import_adds_tracks_and_leaves_source() {
    stems::FakeStemWriter w;
    StemSplitModel m(w, fake_factory());
    Tracks t;
    SourceAudio src = source(2);
    const auto before = src.channels;
    CHECK(m.start(src, "/stems", t));
    CHECK(m.finish() == Phase::Done);
    CHECK(t.added.size() == 4 && m.tracks_added() == 4);
    CHECK(w.files.size() == 4);
    CHECK(m.progress() == 1.0);
    CHECK(src.channels == before);  // the clip the dialog was opened on
    CHECK(t.added[0].name.rfind("Mix ", 0) == 0);
    return 0;
}

int test_cancel_adds_no_tracks() {
    stems::FakeStemWriter w;
    StemSplitModel* self = nullptr;
    StemSplitModel m(w, [&self](Method, int) { return std::make_unique<CancelOnSeparate>(self); });
    self = &m;
    Tracks t;
    CHECK(m.start(source(2), "/stems", t));
    CHECK(m.finish() == Phase::Cancelled);
    CHECK(t.added.empty() && m.tracks_added() == 0);
    CHECK(w.files.empty());
    CHECK(m.notice().empty());
    return 0;
}

int test_failures_add_no_tracks_and_are_held() {
    stems::FakeStemWriter w;
    w.fail_on_write = 1;
    StemSplitModel m(w, fake_factory());
    Tracks t;
    CHECK(m.start(source(2), "/stems", t));
    CHECK(m.finish() == Phase::Failed);
    CHECK(t.added.empty() && w.files.empty());
    CHECK(!m.notice().empty());

    stems::FakeStemWriter w2;
    StemSplitModel m2(w2, fake_factory());
    t.fail = true;
    CHECK(m2.start(source(2), "/stems", t));
    CHECK(m2.finish() == Phase::Failed);
    CHECK(t.added.empty() && w2.files.empty());
    CHECK(m2.notice().find("no room") != std::string::npos);
    return 0;
}

int test_refusals_and_session_reset() {
    stems::FakeStemWriter w;
    StemSplitModel m(w, fake_factory());
    Tracks t;
    m.set_method(Method::VocalCentre);
    CHECK(!m.start(source(1), "/stems", t));
    CHECK(m.notice().find("stereo") != std::string::npos);
    m.dismiss_notice();
    CHECK(m.notice().empty());
    CHECK(!m.start(SourceAudio{}, "/stems", t));
    CHECK(!m.start(source(2), "", t));
    CHECK(!m.notice().empty());
    m.set_method(Method::HarmonicPercussive);
    CHECK(m.start(source(2), "/stems", t));
    CHECK(m.finish() == Phase::Done);
    m.reset();
    CHECK(m.phase() == Phase::Idle && m.tracks_added() == 0 && m.progress() == 0.0);
    return 0;
}

int test_labels_say_not_ml() {
    CHECK(std::string(method_caveat(Method::HarmonicPercussive)).find("not ML") != std::string::npos);
    CHECK(std::string(method_caveat(Method::VocalCentre)).find("not ML") != std::string::npos);
    return 0;
}

}  // namespace

int main() {
    int rc = 0;
    rc |= test_import_adds_tracks_and_leaves_source();
    rc |= test_cancel_adds_no_tracks();
    rc |= test_failures_add_no_tracks_and_are_held();
    rc |= test_refusals_and_session_reset();
    rc |= test_labels_say_not_ml();
    if (!rc) std::puts("ui_stems model: ok");
    return rc;
}
