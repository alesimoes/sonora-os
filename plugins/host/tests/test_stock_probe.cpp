// SPDX-License-Identifier: GPL-3.0-or-later
//
// plugins_host_stock_probe (ADR-0076): every LV2 bundle that a package of
// plugins/content/stock.json installed goes through the host's own scan
// (`sonora-plugin-host --list --json --format lv2 --only-given --bundle DIR`,
// one child per bundle) and must answer usable with an audio output. A bundle
// that makes the host die is a refusal naming it, never an abort of this test.
//
//   test_stock_probe --host PATH --manifest stock.json
//
// With no bundle installed the test FAILS, so an image never ships an empty
// set silently; SONORA_STOCK_OPTIONAL=1 (the PR micro build only) turns that
// into a skip.
#include <sys/wait.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Run {
    int status = -1;
    std::string out;
};

// Argument is passed through the shell inside single quotes.
std::string quote(const std::string& s) {
    std::string q = "'";
    for (char c : s) q += c == '\'' ? std::string("'\''") : std::string(1, c);
    return q + "'";
}

Run run(const std::string& cmd) {
    Run r;
    std::FILE* p = popen((cmd + " 2>/dev/null").c_str(), "r");
    if (!p) return r;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) r.out.append(buf, n);
    r.status = pclose(p);
    return r;
}

std::vector<std::string> manifest_packages(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    std::vector<std::string> out;
    const std::regex re("\"package\"\\s*:\\s*\"([^\"]+)\"");
    for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) out.push_back((*it)[1]);
    return out;
}

std::string lv2_path(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    std::smatch m;
    const std::string text = ss.str();
    if (std::regex_search(text, m, std::regex("\"lv2_path\"\\s*:\\s*\"([^\"]+)\""))) return m[1];
    return "/usr/lib/lv2";
}

// The .lv2 directories a package put under the LV2 path.
std::vector<std::string> bundles_of(const std::string& pkg, const std::string& root) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    const Run r = run("dpkg -L " + quote(pkg));
    std::istringstream in(r.out);
    for (std::string line; std::getline(in, line);) {
        if (line.compare(0, root.size() + 1, root + "/") != 0) continue;
        const std::string rel = line.substr(root.size() + 1);
        const std::size_t slash = rel.find('/');
        const std::string top = rel.substr(0, slash);
        if (top.size() > 4 && top.compare(top.size() - 4, 4, ".lv2") == 0 && seen.insert(top).second)
            out.push_back(root + "/" + top);
    }
    return out;
}

unsigned number_after(const std::string& line, const char* key) {
    const std::size_t at = line.find(key);
    return at == std::string::npos ? 0 : static_cast<unsigned>(std::strtoul(line.c_str() + at + std::strlen(key), nullptr, 10));
}

std::string string_after(const std::string& line, const char* key) {
    const std::size_t at = line.find(key);
    if (at == std::string::npos) return {};
    const std::size_t from = at + std::strlen(key);
    return line.substr(from, line.find('"', from) - from);
}

// Returns the number of plugins found; adds a message per refusal.
int probe_bundle(const std::string& host, const std::string& bundle, std::vector<std::string>& refusals,
                 int& instruments) {
    const Run r = run(quote(host) + " --list --json --format lv2 --only-given --bundle " + quote(bundle));
    if (!WIFEXITED(r.status) || WEXITSTATUS(r.status) != 0) {
        refusals.push_back(bundle + ": the host did not finish the scan (status " + std::to_string(r.status) + ")");
        return 0;
    }
    int plugins = 0;
    std::istringstream in(r.out);
    for (std::string line; std::getline(in, line);) {
        if (line.find("\"uri\":\"") == std::string::npos) continue;
        ++plugins;
        const std::string name = string_after(line, "\"uri\":\"");
        if (line.find("\"usable\":true") == std::string::npos)
            refusals.push_back(name + " (" + bundle + "): not usable: " + string_after(line, "\"reason\":\""));
        else if (number_after(line, "\"audio_out\":") == 0 ||
                 line.find("\"type\":\"audio\",\"direction\":\"out\"") == std::string::npos)
            refusals.push_back(name + " (" + bundle + "): no audio out");
        // ADR-0076 section 3 curates by package, so a package's instruments
        // (zam-plugins' synths, LSP's samplers: audio out, no audio in) are in
        // the set; counted, not refused.
        else if (number_after(line, "\"audio_in\":") == 0)
            ++instruments;
    }
    if (plugins == 0) refusals.push_back(bundle + ": the scan found no plugin");
    return plugins;
}

}  // namespace

int main(int argc, char** argv) {
    std::string host, manifest;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--host")) host = argv[i + 1];
        else if (!std::strcmp(argv[i], "--manifest")) manifest = argv[i + 1];
    }
    if (host.empty() || manifest.empty()) {
        std::cerr << "usage: test_stock_probe --host PATH --manifest stock.json\n";
        return 2;
    }
    const std::string root = lv2_path(manifest);
    std::vector<std::string> refusals;
    int bundles = 0, plugins = 0, instruments = 0;
    for (const std::string& pkg : manifest_packages(manifest)) {
        const std::vector<std::string> found = bundles_of(pkg, root);
        if (found.empty()) refusals.push_back(pkg + ": installed no LV2 bundle under " + root);
        for (const std::string& b : found) {
            ++bundles;
            plugins += probe_bundle(host, b, refusals, instruments);
        }
    }
    const char* optional = std::getenv("SONORA_STOCK_OPTIONAL");
    if (bundles == 0 && optional && !std::strcmp(optional, "1")) {
        std::cout << "stock bundles absent; SONORA_STOCK_OPTIONAL=1, skipped\n";
        return 0;
    }
    for (const std::string& m : refusals) std::cerr << "REFUSED " << m << "\n";
    if (!refusals.empty()) {
        std::cerr << "stock set: " << refusals.size() << " refusal(s) (set SONORA_STOCK_OPTIONAL=1 only in the PR micro build)\n";
        return 1;
    }
    std::cout << "stock set: " << bundles << " bundle(s), " << plugins << " plugin(s) usable\n";
    return 0;
}
