#include "sonora/stems/stem_render.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "sonora/wav_writer.h"

namespace sonora::stems {

namespace {

constexpr double kSeparateShare = 0.8;

// Maps the separator's 0..1 onto the first part of the renderer's progress.
class ScaledSink final : public ProgressSink {
public:
    explicit ScaledSink(ProgressSink* outer) : outer_(outer) {}
    bool report(double fraction) override {
        return !outer_ || outer_->report(fraction * kSeparateShare);
    }

private:
    ProgressSink* outer_;
};

bool set_err(std::string* err, const std::string& m) {
    if (err) *err = m;
    return false;
}

std::string join(const std::string& dir, const std::string& file) {
    if (dir.empty()) return file;
    const char last = dir.back();
    return (last == '/' || last == '\\') ? dir + file : dir + '/' + file;
}

void interleave(const std::vector<float>& planar, int channels, std::int64_t frames,
                std::vector<float>& out) {
    out.resize(static_cast<std::size_t>(channels) * frames);
    for (int c = 0; c < channels; ++c)
        for (std::int64_t i = 0; i < frames; ++i)
            out[i * channels + c] = planar[c * frames + i];
}

}  // namespace

const char* stem_kind_id(StemKind k) {
    switch (k) {
    case StemKind::Vocals: return "vocals";
    case StemKind::Drums: return "drums";
    case StemKind::Bass: return "bass";
    case StemKind::Other: return "other";
    case StemKind::Harmonic: return "harmonic";
    case StemKind::Percussive: return "percussive";
    }
    return "stem";
}

const char* stem_kind_label(StemKind k) {
    switch (k) {
    case StemKind::Vocals: return "Vocals";
    case StemKind::Drums: return "Drums";
    case StemKind::Bass: return "Bass";
    case StemKind::Other: return "Other";
    case StemKind::Harmonic: return "Harmonic";
    case StemKind::Percussive: return "Percussive";
    }
    return "Stem";
}

bool WavStemWriter::write(const std::string& path, const float* interleaved, int channels,
                          std::int64_t frames, int sample_rate, std::string* err) {
    WavWriter w;
    if (!w.open(path, static_cast<uint32_t>(sample_rate), static_cast<uint16_t>(channels),
                WavWriter::Format::Float32, 0))
        return set_err(err, "cannot create " + path + ": " + std::strerror(errno));
    const bool ok = w.write(interleaved, static_cast<std::size_t>(frames));
    const bool closed = w.close();
    if (ok && closed) return true;
    const std::string why = w.failure().empty() ? "write failed" : w.failure();
    std::remove(path.c_str());
    return set_err(err, path + ": " + why);
}

void WavStemWriter::remove(const std::string& path) { std::remove(path.c_str()); }

bool FakeStemWriter::write(const std::string& path, const float* interleaved, int channels,
                           std::int64_t frames, int, std::string* err) {
    if (writes++ == fail_on_write) return set_err(err, "disk full");
    if (files.count(path)) return set_err(err, "exists");
    files[path].assign(interleaved, interleaved + channels * frames);
    return true;
}

void FakeStemWriter::remove(const std::string& path) { files.erase(path); }

bool StemRenderer::render(const StemRenderRequest& req, std::vector<RenderedStem>& out,
                          ProgressSink* progress, std::string* err) {
    out.clear();
    if (req.dest_dir.empty()) return set_err(err, "no destination");
    StemResult res;
    ScaledSink scaled(progress);
    if (!separator_.separate(req.source, res, &scaled, err)) return false;

    int to_write = 0;
    for (const auto& s : res.stems)
        if (!s.empty()) ++to_write;
    if (to_write == 0) return set_err(err, "no stems");
    const std::string base = req.base_name.empty() ? std::string("stem") : req.base_name;
    std::vector<float> buf;
    int written = 0;
    auto rollback = [&](const std::string& why) {
        for (const auto& r : out) writer_.remove(r.path);
        out.clear();
        return set_err(err, why);
    };
    for (std::size_t k = 0; k < res.stems.size(); ++k) {
        if (res.stems[k].empty()) continue;
        const auto kind = static_cast<StemKind>(k);
        const std::string path = join(req.dest_dir, base + "_" + stem_kind_id(kind) + ".wav");
        interleave(res.stems[k], res.channels, res.frames, buf);
        std::string why;
        if (!writer_.write(path, buf.data(), res.channels, res.frames, req.source.sample_rate,
                           &why))
            return rollback(why);
        out.push_back({base + " " + stem_kind_label(kind), path});
        ++written;
        const double f = kSeparateShare + (1.0 - kSeparateShare) * written / to_write;
        if (progress && !progress->report(f)) return rollback("cancelled");
    }
    return true;
}

}  // namespace sonora::stems
