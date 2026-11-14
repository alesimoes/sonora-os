#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "sonora/stems/separators.h"
#include "sonora/stems/stem_render.h"
#include "sonora/wav_reader.h"

using namespace sonora::stems;
namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

// Cancels on the n-th report (1-based).
struct StopAt : ProgressSink {
    explicit StopAt(int n) : n_(n) {}
    int calls = 0;
    double last = 0;
    bool report(double f) override { last = f; return ++calls < n_; }
    int n_;
};

constexpr int kFrames = 512;
constexpr int kChannels = 2;

struct Source {
    std::vector<float> l = std::vector<float>(kFrames), r = std::vector<float>(kFrames);
    const float* in[2];
    Source() {
        for (int i = 0; i < kFrames; ++i) {
            l[i] = 0.7f * std::sin(i * 0.05f) + 0.2f * std::sin(i * 0.9f);
            r[i] = 0.5f * std::cos(i * 0.03f) - 0.1f * ((i % 16) == 0);
        }
        in[0] = l.data();
        in[1] = r.data();
    }
    StemRequest req() const { return StemRequest{in, kChannels, kFrames, 48000}; }
};

fs::path fresh_dir(const char* tag) {
    const fs::path d = fs::temp_directory_path() / (std::string("sonora_stems_") + tag);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

bool dir_empty(const fs::path& d) { return fs::is_empty(d); }

// Real WAV files, read back: the stems sum to the source.
void sums_to_source(StemSeparator& sep, const char* tag) {
    Source src;
    const fs::path dir = fresh_dir(tag);
    WavStemWriter w;
    StemRenderer r(sep, w);
    std::vector<RenderedStem> out;
    std::string err;
    StopAt never(1 << 30);
    CHECK(r.render({src.req(), dir.string(), "Take 1"}, out, &never, &err));
    CHECK(!out.empty());
    CHECK(std::fabs(never.last - 1.0) < 1e-9);
    std::vector<double> sum(kChannels * kFrames, 0.0);
    for (const auto& s : out) {
        CHECK(s.name.rfind("Take 1 ", 0) == 0);
        sonora::WavReader rd;
        std::string e;
        CHECK(rd.open(s.path, e));
        CHECK(rd.channels() == kChannels && rd.frames() == kFrames && rd.sample_rate() == 48000);
        std::vector<float> buf(kChannels * kFrames);
        CHECK(rd.read(0, kFrames, buf.data()) == kFrames);
        for (std::size_t i = 0; i < buf.size(); ++i) sum[i] += buf[i];
    }
    double worst = 0;
    for (int i = 0; i < kFrames; ++i)
        for (int c = 0; c < kChannels; ++c)
            worst = std::fmax(worst, std::fabs(sum[i * kChannels + c] - src.in[c][i]));
    std::printf("%s: %zu stems, max |sum - source| = %g\n", sep.name(), out.size(), worst);
    CHECK(worst < 1e-4);

    // A second render into the same folder refuses to overwrite and leaves
    // the first set as it was.
    std::vector<RenderedStem> again;
    CHECK(!r.render({src.req(), dir.string(), "Take 1"}, again, nullptr, &err));
    CHECK(again.empty());
    for (const auto& s : out) CHECK(fs::exists(s.path));
    fs::remove_all(dir);
}

void cancel_leaves_no_files() {
    Source src;
    FakeSeparator sep;
    WavStemWriter w;
    StemRenderer r(sep, w);
    std::string err;
    // FakeSeparator reports 6 times; the renderer then once per file (4).
    for (int n : {1, 3, 7, 8, 10}) {
        const fs::path dir = fresh_dir("cancel");
        std::vector<RenderedStem> out;
        StopAt stop(n);
        CHECK(!r.render({src.req(), dir.string(), "t"}, out, &stop, &err));
        CHECK(err == "cancelled");
        CHECK(out.empty());
        CHECK(dir_empty(dir));
        fs::remove_all(dir);
    }
}

void write_error_rolls_back() {
    Source src;
    FakeSeparator sep;
    FakeStemWriter w;
    w.fail_on_write = 2;
    StemRenderer r(sep, w);
    std::vector<RenderedStem> out;
    std::string err;
    CHECK(!r.render({src.req(), "/stems", "t"}, out, nullptr, &err));
    CHECK(err == "disk full");
    CHECK(out.empty());
    CHECK(w.files.empty());

    FakeStemWriter ok;
    StemRenderer r2(sep, ok);
    CHECK(r2.render({src.req(), "/stems/", "t"}, out, nullptr, &err));
    CHECK(out.size() == 4);
    CHECK(out[0].name == "t Vocals" && out[0].path == "/stems/t_vocals.wav");
    CHECK(out[3].path == "/stems/t_other.wav");
    CHECK(ok.files.size() == 4);
    CHECK(ok.files[out[0].path].size() == std::size_t(kChannels * kFrames));
    // Interleaved: frame 1, channel 1 is r[1] / 4.
    CHECK(std::fabs(ok.files[out[0].path][3] - src.r[1] / 4) < 1e-7f);

    FakeStemWriter none;
    StemRenderer r3(sep, none);
    CHECK(!r3.render({src.req(), "", "t"}, out, nullptr, &err));
    CHECK(!r3.render({StemRequest{}, "/stems", "t"}, out, nullptr, &err));
    CHECK(none.files.empty() && out.empty());
}

int main() {
    FakeSeparator fake;
    sums_to_source(fake, "fake");
    HpssSeparator hpss;
    sums_to_source(hpss, "hpss");
    CentreSeparator centre;
    sums_to_source(centre, "centre");
    cancel_leaves_no_files();
    write_error_rolls_back();
    return fails ? 1 : 0;
}
