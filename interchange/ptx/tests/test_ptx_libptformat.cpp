// SPDX-License-Identifier: GPL-3.0-or-later
// Registered only with SONORA_WITH_LIBPTFORMAT. No real .ptx fixture is
// committed (third-party session licence), so by default this checks the
// failure paths; with tests/fixtures/session.ptx present it also imports it.
#include <cstdio>
#include <filesystem>

#include "../src/ptx_libptformat_reader.h"

using namespace sonora::interchange;

static int fails = 0;
#define CHECK(c)                                             \
    do {                                                     \
        if (!(c)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #c); \
            ++fails;                                         \
        }                                                    \
    } while (0)

int main() {
    namespace fs = std::filesystem;
    LibPtformatReader reader;
    PtxDocument doc;
    std::string err;
    CHECK(!reader.read("does-not-exist.ptx", doc, err));
    CHECK(err.find("does-not-exist.ptx") != std::string::npos);

    const fs::path junk = fs::temp_directory_path() / "sonora_not_a_session.ptx";
    if (std::FILE* f = std::fopen(junk.string().c_str(), "wb")) {
        std::fputs("junk", f);
        std::fclose(f);
    }
    err.clear();
    CHECK(!reader.read(junk.string(), doc, err));
    CHECK(!err.empty());
    fs::remove(junk);

    PtxDocument m;
    m.sources.push_back({"kick", "no-such-kick.wav", 10});
    err.clear();
    CHECK(!copy_ptx_media(m, ".", (fs::temp_directory_path() / "sonora_ptx_media").string(), err));
    CHECK(err.find("no-such-kick.wav") != std::string::npos);

    CHECK(make_ptx_importer()->can_read("a.ptx"));

    const fs::path fixture = fs::path(FIXTURE_DIR) / "session.ptx";
    if (fs::exists(fixture)) {
        err.clear();
        CHECK(reader.read(fixture.string(), doc, err));
        CHECK(doc.sample_rate > 0);
        CHECK(!doc.tracks.empty());
    }
    return fails ? 1 : 0;
}
