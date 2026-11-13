#include <cstddef>

#include "sonora/stems/separators.h"
#include "sonora/stems/stem_separator.h"

namespace sonora::stems {

bool FakeSeparator::supports(StemKind k) const {
    return k == StemKind::Vocals || k == StemKind::Drums || k == StemKind::Bass ||
           k == StemKind::Other;
}

bool FakeSeparator::separate(const StemRequest& r, StemResult& out, ProgressSink* p,
                             std::string* err) {
    auto fail = [&](const char* m) {
        if (err) *err = m;
        return false;
    };
    if (!r.in || r.channels <= 0 || r.frames < 0 || r.sample_rate <= 0)
        return fail("bad request");
    constexpr int n = 4;
    out.channels = r.channels;
    out.frames = r.frames;
    out.stems.assign(kStemKindCount, {});
    for (int k = 0; k < kStemKindCount; ++k) {
        if (!supports(static_cast<StemKind>(k))) continue;
        out.stems[k].resize(static_cast<std::size_t>(r.channels) * r.frames);
        for (int c = 0; c < r.channels; ++c)
            for (std::int64_t i = 0; i < r.frames; ++i)
                out.stems[k][c * r.frames + i] = r.in[c][i] / n;
        if (p && !p->report(double(k + 1) / kStemKindCount)) {
            out.stems.clear();
            return fail("cancelled");
        }
    }
    return true;
}

}  // namespace sonora::stems
