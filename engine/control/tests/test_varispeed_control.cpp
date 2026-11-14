// SPDX-License-Identifier: GPL-3.0-or-later
//
// transport.varispeed's control path: parse, then Session::set_varispeed.
#include <cstdio>
#include <filesystem>
#include <string>

#include "sonora/session.h"
#include "sonora/varispeed_command.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

int main() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "sonora_test_varispeed_control";
    std::error_code ec;
    fs::remove_all(dir, ec);
    sonora::SessionConfig cfg;
    cfg.dir = dir.string();
    cfg.track_count = 2;
    cfg.ring_seconds = 0.25;
    sonora::Session s(cfg);

    std::string why;
    CHECK(sonora::apply_varispeed(s, "1.25", why));
    CHECK(s.varispeed() == 1.25f);
    CHECK(sonora::apply_varispeed(s, "1", why));
    CHECK(s.varispeed() == 1.0f);

    CHECK(sonora::apply_varispeed(s, "0.5", why));
    for (const char* bad : {"", "nan", "inf", "-1", "0.49", "2.01", "9999999999999999999", "1e0", "abc"}) {
        why.clear();
        CHECK(!sonora::apply_varispeed(s, bad, why));
        CHECK(!why.empty());
        CHECK(s.varispeed() == 0.5f);
    }
    fs::remove_all(dir, ec);
    return 0;
}
