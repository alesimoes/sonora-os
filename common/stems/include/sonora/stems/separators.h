#pragma once
#include <memory>
#include <vector>

#include "sonora/stems/stem_separator.h"

namespace sonora::stems {

// Median-filter harmonic/percussive split (Harmonic + Percussive == input).
class HpssSeparator final : public StemSeparator {
public:
    const char* name() const override { return "hpss"; }
    bool supports(StemKind kind) const override;
    bool separate(const StemRequest& req, StemResult& out, ProgressSink* progress,
                  std::string* err) override;
};

// Mid/side mask: Vocals = centre estimate, Other = remainder. Needs stereo;
// built for a mono source it supports nothing.
class CentreSeparator final : public StemSeparator {
public:
    explicit CentreSeparator(int channels = 2) : channels_(channels) {}
    const char* name() const override { return "centre"; }
    bool supports(StemKind kind) const override;
    bool separate(const StemRequest& req, StemResult& out, ProgressSink* progress,
                  std::string* err) override;

private:
    int channels_;
};

// Separators available in this build: hpss, then centre (ADR-0165).
std::vector<std::unique_ptr<StemSeparator>> default_separators();

}  // namespace sonora::stems
