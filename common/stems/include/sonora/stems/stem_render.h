#pragma once
// Stems rendered to new files (ADR-0165 step 3, ADR-0223). Worker thread only:
// it separates, allocates and writes files, for seconds at a time.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "sonora/stems/stem_separator.h"

namespace sonora::stems {

// Lower-case, file-name safe: "vocals", "drums", ...
const char* stem_kind_id(StemKind kind);
// For a track name: "Vocals", "Drums", ...
const char* stem_kind_label(StemKind kind);

// The import-as-tracks hand-off: one entry per file, in StemKind order.
struct RenderedStem {
    std::string name;
    std::string path;
};

// Where a stem's samples go. Injected so the renderer is tested without a disk.
class StemFileWriter {
public:
    virtual ~StemFileWriter() = default;
    // Creates `path` and writes interleaved frames. Must refuse a path that
    // exists: the renderer never overwrites, so the original stays untouched.
    // On failure leaves no file it created (err set if non-null).
    virtual bool write(const std::string& path, const float* interleaved, int channels,
                       std::int64_t frames, int sample_rate, std::string* err) = 0;
    // Deletes a file this writer created; for the rollback on cancel or error.
    virtual void remove(const std::string& path) = 0;
};

// 32-bit float BWF through common/audiofile, so stems sum back to the source
// bit for bit up to float rounding.
class WavStemWriter final : public StemFileWriter {
public:
    bool write(const std::string& path, const float* interleaved, int channels,
               std::int64_t frames, int sample_rate, std::string* err) override;
    void remove(const std::string& path) override;
};

// In-memory writer for tests. `fail_on_write` = index (0-based) of the write
// call that fails, -1 for none.
class FakeStemWriter final : public StemFileWriter {
public:
    bool write(const std::string& path, const float* interleaved, int channels,
               std::int64_t frames, int sample_rate, std::string* err) override;
    void remove(const std::string& path) override;

    std::map<std::string, std::vector<float>> files;
    int writes = 0;
    int fail_on_write = -1;
};

struct StemRenderRequest {
    StemRequest source;
    std::string dest_dir;   // must exist; files are created inside it
    std::string base_name;  // file "<base>_<id>.wav", track "<base> <Label>"
};

class StemRenderer {
public:
    StemRenderer(StemSeparator& separator, StemFileWriter& writer)
        : separator_(separator), writer_(writer) {}

    // Separates, then writes one file per supported stem. Progress runs 0..0.8
    // for separation and on to 1 across the files; report() false cancels.
    // On false (err "cancelled" or the cause) every file it created is
    // removed and `out` is empty.
    bool render(const StemRenderRequest& req, std::vector<RenderedStem>& out,
                ProgressSink* progress, std::string* err);

private:
    StemSeparator& separator_;
    StemFileWriter& writer_;
};

}  // namespace sonora::stems
