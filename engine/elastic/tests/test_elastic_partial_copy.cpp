// SPDX-License-Identifier: GPL-3.0-or-later
//
// The elastic copy is removed on every exit that does not place it, a throw
// included: std::bad_alloc from write_copy's block buffer used to unwind past
// render_elastic's cleanup, and ElasticJob's catch saw no path to remove, so
// a partial "<track>_elastic_NN.wav" stayed in Audio Files (QA r1380).
#include <cstdio>
#include <filesystem>
#include <new>
#include <string>
#include <vector>

#include "elastic_rig.h"
#include "sonora/elastic/partial_copy.h"
#include "sonora/wav_writer.h"

namespace {

using sonora::WavWriter;
using sonora::elastic::PartialCopy;

std::string fresh_dir(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

bool open_some(WavWriter& w, const std::string& path) {
    if (!w.open(path, 48000, 2, WavWriter::Format::Int24, 0)) return false;
    const std::vector<float> frames(2 * 64, 0.25f);
    return w.write(frames.data(), 64);
}

int test_throw_removes_the_copy() {
    const std::string path = fresh_dir("test_elastic_partial_throw") + "/Drums_elastic_01.wav";
    WavWriter w;
    CHECK(open_some(w, path));
    bool caught = false;
    try {
        PartialCopy guard(w, path);
        throw std::bad_alloc();
    } catch (const std::bad_alloc&) {
        caught = true;
    }
    CHECK(caught);
    CHECK(!std::filesystem::exists(path));
    return 0;
}

int test_early_return_removes_the_copy() {
    const std::string path = fresh_dir("test_elastic_partial_return") + "/Drums_elastic_01.wav";
    WavWriter w;
    CHECK(open_some(w, path));
    { PartialCopy guard(w, path); }
    CHECK(!std::filesystem::exists(path));
    return 0;
}

int test_kept_copy_stays() {
    const std::string path = fresh_dir("test_elastic_partial_keep") + "/Drums_elastic_01.wav";
    WavWriter w;
    CHECK(open_some(w, path));
    CHECK(w.close());
    {
        PartialCopy guard(w, path);
        guard.keep();
    }
    CHECK(std::filesystem::exists(path));
    return 0;
}

}  // namespace

int main() {
    if (test_throw_removes_the_copy() != 0) return 1;
    if (test_early_return_removes_the_copy() != 0) return 1;
    if (test_kept_copy_stays() != 0) return 1;
    std::printf("elastic_partial_copy: ok\n");
    return 0;
}
