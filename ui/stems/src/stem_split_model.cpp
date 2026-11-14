// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/ui_stems/stem_split_model.h"

#include "sonora/stems/separators.h"

namespace sonora::ui_stems {

const char* method_label(Method m) {
    return m == Method::VocalCentre ? "Vocal / centre" : "Harmonic / percussive";
}

const char* method_caveat(Method m) {
    return m == Method::VocalCentre
               ? "Signal-processing estimate of the centre image, not ML quality. Needs stereo."
               : "Signal-processing estimate, not ML quality. Harmonic + percussive = source.";
}

SeparatorFactory default_separator_factory() {
    return [](Method m, int channels) -> std::unique_ptr<stems::StemSeparator> {
        if (m == Method::VocalCentre) return std::make_unique<stems::CentreSeparator>(channels);
        return std::make_unique<stems::HpssSeparator>();
    };
}

namespace {
class Sink final : public stems::ProgressSink {
public:
    Sink(std::atomic<double>& p, std::atomic<bool>& c) : p_(p), c_(c) {}
    bool report(double f) override {
        p_.store(f);
        return !c_.load();
    }

private:
    std::atomic<double>& p_;
    std::atomic<bool>& c_;
};
}  // namespace

StemSplitModel::~StemSplitModel() {
    cancel_.store(true);
    if (worker_.joinable()) worker_.join();
}

void StemSplitModel::reset() {
    if (worker_.joinable()) {
        cancel_.store(true);
        worker_.join();
    }
    cancel_.store(false);
    progress_.store(0.0);
    phase_.store(Phase::Idle);
    importer_ = nullptr;
    rendered_.clear();
    error_.clear();
    notice_.clear();
    tracks_added_ = 0;
}

bool StemSplitModel::refuse(const std::string& why) {
    notice_ = why;
    return false;
}

bool StemSplitModel::start(SourceAudio source, const std::string& dest_dir,
                           TrackImporter& importer) {
    if (running()) return refuse("A split is already running");
    if (source.channels.empty() || source.channels[0].empty() || source.sample_rate <= 0)
        return refuse("Nothing to split: the region has no audio");
    if (dest_dir.empty()) return refuse("No folder for the stem files");
    if (method_ == Method::VocalCentre && source.channels.size() != 2)
        return refuse("Vocal / centre needs a stereo region");
    reset();
    importer_ = &importer;
    phase_.store(Phase::Running);
    worker_ = std::thread(&StemSplitModel::work, this, std::move(source), dest_dir);
    return true;
}

void StemSplitModel::work(SourceAudio source, std::string dest_dir) {
    const int ch = static_cast<int>(source.channels.size());
    auto sep = factory_(method_, ch);
    std::vector<const float*> in;
    for (auto& c : source.channels) in.push_back(c.data());
    stems::StemRenderRequest req;
    req.source.in = in.data();
    req.source.channels = ch;
    req.source.frames = static_cast<std::int64_t>(source.channels[0].size());
    req.source.sample_rate = source.sample_rate;
    req.dest_dir = dest_dir;
    req.base_name = source.name.empty() ? "Region" : source.name;
    Sink sink(progress_, cancel_);
    stems::StemRenderer renderer(*sep, writer_);
    std::string err;
    const bool ok = renderer.render(req, rendered_, &sink, &err);
    if (ok) {
        phase_.store(Phase::Done);
    } else if (cancel_.load()) {
        phase_.store(Phase::Cancelled);
    } else {
        error_ = err;
        phase_.store(Phase::Failed);
    }
}

Phase StemSplitModel::finish() {
    if (worker_.joinable()) worker_.join();
    if (phase_.load() == Phase::Done && importer_ && tracks_added_ == 0) {
        std::string err;
        if (importer_->import_stems(rendered_, &err)) {
            tracks_added_ = static_cast<int>(rendered_.size());
            progress_.store(1.0);
        } else {
            error_ = err.empty() ? "Could not add the tracks" : err;
            for (const auto& s : rendered_) writer_.remove(s.path);
            phase_.store(Phase::Failed);
        }
    }
    if (phase_.load() == Phase::Failed) notice_ = "Split failed: " + error_;
    return phase_.load();
}

std::string StemSplitModel::status() const {
    switch (phase_.load()) {
        case Phase::Idle: return "";
        case Phase::Running: return "Splitting...";
        case Phase::Done: return "Added " + std::to_string(tracks_added_) + " tracks";
        case Phase::Cancelled: return "Cancelled: no tracks added";
        case Phase::Failed: return "Split failed";
    }
    return "";
}

}  // namespace sonora::ui_stems
