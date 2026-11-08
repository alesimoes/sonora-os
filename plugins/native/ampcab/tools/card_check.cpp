// SPDX-License-Identifier: GPL-3.0-or-later
//
// ampcab-card-check [--manifest M] [--cards DIR] [--files DIR] [--deny F]
// The build check of ADR-0069 s5: every manifest entry has a valid card, its
// file exists, and the file's SHA-256 and size match card and manifest; no
// capture or IR file in the files directory lacks a card. One line per
// refusal on stdout, exit 1 if any. Build-time only, never in a `process`.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "sonora/ampcab/capture_card.h"
#include "sonora/json/json.h"

namespace fs = std::filesystem;
using namespace sonora::ampcab;

namespace {

bool read_file(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool plain_name(const std::string& s) {
    return !s.empty() && s.find_first_of("/\\") == std::string::npos &&
           s.find("..") == std::string::npos;
}

struct Checker {
    std::vector<std::string> refusals;
    std::vector<std::string> deny;
    fs::path cards, files;
    std::uint64_t total = 0;
    std::set<std::string> named_files;

    void refuse(const std::string& id, const std::string& msg) {
        refusals.push_back(id + ": " + msg);
    }

    void entry(const sonora::json::Value& e) {
        const auto* idv = e.find("id");
        const std::string id = idv && idv->is_string() ? idv->text() : "";
        if (!plain_name(id)) return refuse(id.empty() ? "<entry>" : id, "missing or bad id");
        std::string md;
        if (!read_file(cards / (id + ".md"), md)) return refuse(id, "card not found");
        CaptureCard card;
        const auto shape = parse_card(md, card);
        for (const auto& r : shape) refuse(id, "card: " + r);
        if (!shape.empty()) return;
        for (const auto& r : validate_card(card, deny)) refuse(id, "card: " + r);
        if (card.id != id) refuse(id, "card id is " + card.id);
        named_files.insert(card.file.name);
        manifest_fields(id, e, card);
        file_check(id, card);
    }

    void manifest_fields(const std::string& id, const sonora::json::Value& e,
                         const CaptureCard& card) {
        const auto* h = e.find("sha256");
        if (!h || !h->is_string() || h->text() != card.file.sha256)
            refuse(id, "manifest sha256 differs from card");
        std::int64_t size = -1;
        const auto* s = e.find("size");
        if (!s || !s->to_int64(size) || size < 0 ||
            static_cast<std::uint64_t>(size) != card.file.size)
            refuse(id, "manifest size differs from card");
    }

    void file_check(const std::string& id, const CaptureCard& card) {
        if (!plain_name(card.file.name)) return refuse(id, "file name is not a plain name");
        std::string bytes;
        if (!read_file(files / card.file.name, bytes))
            return refuse(id, "file " + card.file.name + " not found");
        total += bytes.size();
        if (bytes.size() > kMaxCaptureFileBytes) refuse(id, "file above 25 MB");
        for (const auto& r : check_capture_file(card, bytes)) refuse(id, r);
    }

    void orphans() {
        std::error_code ec;
        for (fs::directory_iterator it(files, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            std::string ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext != ".nam" && ext != ".wav" && ext != ".flac" && ext != ".aiff") continue;
            const std::string name = it->path().filename().string();
            if (!named_files.count(name)) refuse(name, "file has no card in the manifest");
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    fs::path manifest = AMPCAB_DEFAULT_MANIFEST;
    fs::path deny_path = AMPCAB_DEFAULT_DENY;
    Checker c;
    c.cards = AMPCAB_DEFAULT_CARDS;
    c.files = AMPCAB_DEFAULT_FILES;
    for (int i = 1; i < argc; i += 2) {
        const std::string a = argv[i];
        if (i + 1 >= argc) { std::cerr << "missing value for " << a << "\n"; return 2; }
        if (a == "--manifest") manifest = argv[i + 1];
        else if (a == "--cards") c.cards = argv[i + 1];
        else if (a == "--files") c.files = argv[i + 1];
        else if (a == "--deny") deny_path = argv[i + 1];
        else { std::cerr << "unknown option " << a << "\n"; return 2; }
    }
    // A missing deny list is not an empty one (ADR-0069 Consequences).
    std::string text;
    if (!read_file(deny_path, text)) {
        std::cout << "deny list " << deny_path.string() << " not found\n";
        return 1;
    }
    c.deny = parse_name_deny(text);
    if (c.deny.empty()) {
        std::cout << "deny list is empty\n";
        return 1;
    }

    sonora::json::Value m;
    sonora::json::Error err;
    if (!read_file(manifest, text) || !sonora::json::parse(text, m, err)) {
        std::cout << "manifest " << manifest.string() << " unreadable: " << err.message << "\n";
        return 1;
    }
    const auto* entries = m.find("entries");
    if (!entries || !entries->is_array()) {
        std::cout << "manifest has no entries array\n";
        return 1;
    }
    for (const auto& e : entries->items()) c.entry(e);
    c.orphans();
    if (c.total > kMaxManifestTotalBytes) c.refuse("manifest", "files total above 100 MB");
    for (const auto& r : c.refusals) std::cout << r << "\n";
    return c.refusals.empty() ? 0 : 1;
}
