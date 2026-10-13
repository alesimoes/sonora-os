// SPDX-License-Identifier: GPL-3.0-or-later
//
// Writes a bundle's generated files, from the same parameter table and DPF
// export code as the binary itself (ADR-0011 §2.4: one table, everything
// generated from it):
// - with --lv2 NAME, manifest.ttl and NAME.ttl into the current directory;
// - with --descriptor PATH, the JSON descriptor the Sonora UI builds the
//   editor from (§2.6), to PATH.
// plugins/native/format/CMakeLists.txt says why this is an executable and
// not DPF's dlopen()-based TTL generator.
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "sonora/native/format/entry.h"
#include "sonora/native/state.h"

extern "C" void lv2_generate_ttl(const char* basename);

int main(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--lv2") == 0) {
            lv2_generate_ttl(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--descriptor") == 0) {
            const auto p = sonora::native::format::make_bundle_processor();
            // Index 0 is the bypass, which DPF's LV2 exporter writes as the
            // standard lv2:enabled port, inverted (plugin.cpp).
            const sonora::native::FormatSymbol lv2_bypass{0, "lv2", "lv2_enabled", true};
            const std::string json = sonora::native::descriptor_json(p->descriptor(), &lv2_bypass, 1);
            std::FILE* f = std::fopen(argv[i + 1], "wb");
            if (f == nullptr || std::fwrite(json.data(), 1, json.size(), f) != json.size() ||
                std::fclose(f) != 0) {
                std::fprintf(stderr, "%s: cannot write %s\n", argv[0], argv[i + 1]);
                return 1;
            }
        } else {
            std::fprintf(stderr, "usage: %s [--lv2 NAME] [--descriptor PATH]...\n", argv[0]);
            return 2;
        }
    }
    return 0;
}
