#pragma once
// Stem separation contract (ADR-0165). Offline, worker/disk thread only: never
// call separate() from the audio thread, it allocates and runs for seconds.
#include <cstdint>
#include <string>
#include <vector>

namespace sonora::stems {

enum class StemKind { Vocals, Drums, Bass, Other, Harmonic, Percussive };
constexpr int kStemKindCount = 6;

struct StemRequest {
    const float* const* in = nullptr;  // in[channel][frame], not owned
    int channels = 0;
    std::int64_t frames = 0;
    int sample_rate = 0;
};

// One vector per StemKind, channel-major: index = channel * frames + frame.
// Kinds the separator does not support are left empty.
struct StemResult {
    std::vector<std::vector<float>> stems;
    int channels = 0;
    std::int64_t frames = 0;
};

// Returning false from report() cancels the job.
class ProgressSink {
public:
    virtual ~ProgressSink() = default;
    virtual bool report(double fraction) = 0;
};

class StemSeparator {
public:
    virtual ~StemSeparator() = default;
    virtual const char* name() const = 0;
    virtual bool supports(StemKind kind) const = 0;
    // False on error (err set if non-null) or cancellation (err "cancelled").
    virtual bool separate(const StemRequest& req, StemResult& out,
                          ProgressSink* progress, std::string* err) = 0;
};

// Splits by identity: every supported kind = input / n, so the sum equals the
// input. A test double, not a separator.
class FakeSeparator final : public StemSeparator {
public:
    const char* name() const override { return "fake"; }
    bool supports(StemKind kind) const override;
    bool separate(const StemRequest& req, StemResult& out, ProgressSink* progress,
                  std::string* err) override;
};

}  // namespace sonora::stems
