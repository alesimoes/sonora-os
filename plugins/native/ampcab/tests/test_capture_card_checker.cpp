// SPDX-License-Identifier: GPL-3.0-or-later
//
// ampcab-card-check end to end (ADR-0069 s5) on tiny synthetic cards and
// files written for the test; no real capture. One case per refusal in the
// brief, each asserting the exit code and that the output names the culprit.
// Usage: test <tool> <deny list> <scratch dir> <tests/cards dir>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "sonora/ampcab/capture_card.h"
#include "sonora/ampcab/capture_card_fake.h"
#include "sonora/sha256.h"

namespace fs = std::filesystem;
using namespace sonora::ampcab;

namespace {

int failures = 0;

void expect(bool ok, const std::string& what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

void write(const fs::path& p, const std::string& s) {
    std::ofstream(p, std::ios::binary) << s;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct Result {
    int code = -1;
    std::string out;
};

// A fixture directory: cards/, files/ and a manifest built from the cards.
struct Fixture {
    fs::path root;
    std::vector<CaptureCard> cards;

    explicit Fixture(const fs::path& r) : root(r) {
        fs::remove_all(root);
        fs::create_directories(root / "cards");
        fs::create_directories(root / "files");
    }

    // Adds a card whose hash and size describe `bytes`, and writes `bytes`
    // as its file; the caller may then break the card or the file.
    CaptureCard& add(CaptureCard c, const std::string& bytes) {
        c.file.sha256 = sonora::sha256_hex(bytes);
        c.file.size = bytes.size();
        write(root / "files" / c.file.name, bytes);
        cards.push_back(c);
        return cards.back();
    }

    void write_cards() const {
        std::string entries;
        for (const auto& c : cards) {
            write(root / "cards" / (c.id + ".md"), format_card(c));
            if (!entries.empty()) entries += ",";
            entries += "{\"id\":\"" + c.id + "\",\"version\":\"1\",\"sha256\":\"" +
                       c.file.sha256 + "\",\"size\":" + std::to_string(c.file.size) +
                       ",\"card\":\"brain/models/" + c.id + ".md\"}";
        }
        write(root / "manifest.json", "{\"entries\":[" + entries + "]}");
    }

    Result run_tool(const std::string& tool, const std::string& deny) const {
        const fs::path log = root / "out.txt";
        const std::string cmd = "\"" + tool + "\" --deny \"" + deny + "\" --manifest \"" +
                                (root / "manifest.json").string() + "\" --cards \"" +
                                (root / "cards").string() + "\" --files \"" +
                                (root / "files").string() + "\" > \"" + log.string() + "\"";
        Result r;
        r.code = std::system(cmd.c_str()) == 0 ? 0 : 1;
        r.out = slurp(log);
        return r;
    }

    Result run(const std::string& tool, const std::string& deny) const {
        write_cards();
        return run_tool(tool, deny);
    }
};

CaptureCard numbered(CaptureCard c, int n) {
    c.id += "-" + std::to_string(n);
    c.file.name = c.id + ".nam";
    return c;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) return 2;
    const std::string tool = argv[1], real_deny = argv[2];
    const fs::path scratch = argv[3], cards_dir = argv[4];
    const CaptureCard cap = valid_capture_card();
    const CaptureCard ir = valid_ir_card();

    // A refusal case: exit 1 and the output holds `culprit`.
    auto refused = [&](const char* name, Fixture& f, const std::string& culprit,
                       const std::string& deny = "") {
        const Result r = f.run(tool, deny.empty() ? real_deny : deny);
        expect(r.code == 1, std::string(name) + ": exits 1");
        expect(r.out.find(culprit) != std::string::npos,
               std::string(name) + ": output names '" + culprit + "', got: " + r.out);
    };

    {
        Fixture f(scratch / "good");
        f.add(cap, "");
        f.add(ir, "");
        const Result r = f.run(tool, real_deny);
        expect(r.code == 0, "good capture and good IR pass: " + r.out);
    }
    {
        Fixture f(scratch / "rented");
        f.add(ir, "").unit.rental_terms_allow_recording = false;
        refused("rented without terms", f, ir.id);
    }
    {
        Fixture f(scratch / "third-party");
        auto& c = f.add(cap, "");
        c.third_party.present = true;
        c.third_party.licence_path.clear();
        refused("third_party without licence", f, cap.id);
    }
    {
        Fixture f(scratch / "plugin");
        f.add(cap, "").source = CaptureSource::plugin;
        refused("plugin source", f, cap.id);
    }
    {
        Fixture f(scratch / "bad-hash");
        f.add(cap, "");
        write(f.root / "files" / cap.file.name, "x");
        refused("bad hash", f, cap.id);
    }
    {
        Fixture f(scratch / "oversized");
        f.add(cap, std::string(kMaxCaptureFileBytes + 1, 'a'));
        refused("oversized file", f, "25 MB");
    }
    {
        // Six files of 17 MB: each under the file bound, 102 MB together.
        Fixture f(scratch / "total");
        const std::string body(17'000'000, 'b');
        for (int i = 0; i < 6; ++i) f.add(numbered(cap, i), body);
        refused("100 MB total", f, "100 MB");
    }
    {
        Fixture f(scratch / "orphan");
        f.add(cap, "");
        write(f.root / "files" / "stray-capture.wav", "");
        refused("orphan file", f, "stray-capture.wav");
    }
    {
        // A test-only deny list with a fictitious model number (ADR-0011 s5).
        Fixture f(scratch / "deny");
        f.add(cap, "").name = "ZX-900 style crunch";
        write(f.root / "deny.txt", "# fictitious\nzx900\n");
        refused("deny-listed name", f, cap.id, (f.root / "deny.txt").string());
    }
    {
        Fixture f(scratch / "esr");
        f.add(cap, "").capture.has_esr_held_out = false;
        refused("missing ESR", f, "esr_held_out");
    }
    {
        // The unfilled template, as the card a manifest entry names.
        Fixture f(scratch / "template");
        const std::string md = slurp(cards_dir / "template.md");
        expect(!md.empty(), "template card fixture is readable");
        write(f.root / "cards" / "tpl.md", md);
        write(f.root / "manifest.json",
              "{\"entries\":[{\"id\":\"tpl\",\"version\":\"1\",\"sha256\":\"\",\"size\":0,"
              "\"card\":\"brain/models/tpl.md\"}]}");
        const Result r = f.run_tool(tool, real_deny);
        expect(r.code == 1, "template card is refused");
        expect(r.out.find("tpl") != std::string::npos,
               "template refusal names the card: " + r.out);
    }

    fs::remove_all(scratch);
    return failures == 0 ? 0 : 1;
}
