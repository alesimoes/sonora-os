// SPDX-License-Identifier: GPL-3.0-or-later
//
// Runs ampcab-card-check on fixtures built in a scratch directory: a clean
// one (exit 0), a wrong hash (exit 1, names the id) and a capture file with
// no card (exit 1, names the file). Usage: test <tool> <deny list> <scratch>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "sonora/ampcab/capture_card.h"
#include "sonora/ampcab/capture_card_fake.h"

namespace fs = std::filesystem;
using namespace sonora::ampcab;

namespace {

int failures = 0;

void write(const fs::path& p, const std::string& s) {
    std::ofstream(p, std::ios::binary) << s;
}

// Returns the tool's exit code, its output left in `out`.
int run(const std::string& tool, const std::string& deny, const fs::path& root,
        std::string& out) {
    const fs::path log = root / "out.txt";
    const std::string cmd = "\"" + tool + "\" --deny \"" + deny + "\" --manifest \"" +
                            (root / "manifest.json").string() + "\" --cards \"" +
                            (root / "cards").string() + "\" --files \"" +
                            (root / "files").string() + "\" > \"" + log.string() + "\"";
    const int rc = std::system(cmd.c_str());
    std::ifstream in(log);
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return rc == 0 ? 0 : 1;
}

void expect(bool ok, const char* what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

void fixture(const fs::path& root, bool wrong_hash, bool orphan) {
    fs::remove_all(root);
    fs::create_directories(root / "cards");
    fs::create_directories(root / "files");
    const CaptureCard card = valid_capture_card();
    write(root / "cards" / (card.id + ".md"), format_card(card));
    write(root / "files" / card.file.name, wrong_hash ? "x" : "");
    write(root / "manifest.json",
          "{\"entries\":[{\"id\":\"" + card.id + "\",\"version\":\"1\",\"sha256\":\"" +
              card.file.sha256 + "\",\"size\":0,\"card\":\"brain/models/" + card.id +
              ".md\"}]}");
    if (orphan) write(root / "files" / "stray-capture.nam", "");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    const std::string tool = argv[1], deny = argv[2];
    const fs::path root = fs::path(argv[3]);
    std::string out;
    const std::string id = valid_capture_card().id;

    fixture(root, false, false);
    expect(run(tool, deny, root, out) == 0, "clean fixture passes");

    fixture(root, true, false);
    expect(run(tool, deny, root, out) == 1, "wrong hash exits 1");
    expect(out.find(id) != std::string::npos, "wrong hash names the id");

    fixture(root, false, true);
    expect(run(tool, deny, root, out) == 1, "file without card exits 1");
    expect(out.find("stray-capture.nam") != std::string::npos, "orphan names the file");

    fixture(root, false, false);
    fs::remove(root / "files" / valid_capture_card().file.name);
    expect(run(tool, deny, root, out) == 1, "missing file exits 1");

    fs::remove_all(root);
    return failures == 0 ? 0 : 1;
}
