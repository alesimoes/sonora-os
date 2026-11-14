// SPDX-License-Identifier: GPL-3.0-or-later
//
// The template list (ADR-0146 §1, §3): one category level, a template
// folder never read as a category, the version read from the head, and a
// folder whose file is no session file left out.
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "model_test.h"
#include "sonora/model/session_file.h"
#include "sonora/model/template_list.h"

using namespace model_test;

namespace {

// A folder tree on a map: a folder is a path in `dirs`, a file a path in
// `files`. Reads are counted so a test can see nothing past the head is
// deserialized and no folder below a category is walked.
class MapFolder : public TemplateFolder {
public:
    std::set<std::string> dirs;
    std::map<std::string, std::string> files;
    std::vector<std::string> walked;

    std::vector<std::string> subfolders(const std::string& folder) override {
        walked.push_back(folder);
        std::vector<std::string> out;
        const std::string prefix = folder + "/";
        for (const std::string& d : dirs) {
            if (d.compare(0, prefix.size(), prefix) != 0) continue;
            const std::string rest = d.substr(prefix.size());
            if (rest.find('/') == std::string::npos) out.push_back(rest);
        }
        return out;
    }
    bool read_file(const std::string& file, std::string& bytes) override {
        const auto it = files.find(file);
        if (it == files.end()) return false;
        bytes = it->second;
        return true;
    }
    void add_template(const std::string& dir, const std::string& name, std::int64_t version) {
        add_dirs(dir + "/" + name);
        files[dir + "/" + name + "/" + name + ".sonora"] =
            "{\"sonora_session\": " + std::to_string(version) + ", \"rest\": \"not read\"}";
    }
    void add_dirs(const std::string& path) {
        for (std::size_t at = path.find('/', 1); at != std::string::npos; at = path.find('/', at + 1))
            dirs.insert(path.substr(0, at));
        dirs.insert(path);
    }
};

const TemplateInfo* find(const std::vector<TemplateInfo>& list, const std::string& category,
                         const std::string& name) {
    for (const TemplateInfo& t : list)
        if (t.category == category && t.name == name) return &t;
    return nullptr;
}

int test_categories_one_level() {
    MapFolder f;
    f.add_template("/t", "Plain", kFormatVersion);
    f.add_template("/t/Band", "Drums", kFormatVersion);
    f.add_template("/t/Band", "Bass", 18);
    // A category inside a category is not one: its template is not listed.
    f.add_template("/t/Band/Deeper", "Hidden", kFormatVersion);
    f.add_dirs("/t/Empty");

    const std::vector<TemplateInfo> list = list_templates(f, "/t");
    CHECK(list.size() == 3);
    CHECK(list[0].name == "Plain" && list[0].category.empty());
    CHECK(list[0].file == "/t/Plain/Plain.sonora");
    CHECK(list[1].name == "Bass" && list[1].category == "Band");
    CHECK(list[1].file == "/t/Band/Bass/Bass.sonora");
    CHECK(list[1].file_format_version == 18 && !list[1].newer_than_build);
    CHECK(list[2].name == "Drums" && list[2].category == "Band");
    CHECK(find(list, "Band", "Hidden") == nullptr);
    CHECK(find(list, "Deeper", "Hidden") == nullptr);
    for (const std::string& w : f.walked) CHECK(w != "/t/Band/Deeper");
    return 0;
}

int test_a_template_is_never_a_category() {
    MapFolder f;
    f.add_template("/t", "Mixing", kFormatVersion);
    f.add_template("/t/Mixing", "Inner", kFormatVersion);
    const std::vector<TemplateInfo> list = list_templates(f, "/t");
    CHECK(list.size() == 1);
    CHECK(list[0].name == "Mixing" && list[0].category.empty());
    return 0;
}

int test_newer_than_build_is_listed_and_flagged() {
    MapFolder f;
    f.add_template("/t", "Future", kFormatVersion + 1);
    f.add_template("/t/Band", "Later", kFormatVersion + 7);
    const std::vector<TemplateInfo> list = list_templates(f, "/t");
    CHECK(list.size() == 2);
    const TemplateInfo* future = find(list, "", "Future");
    CHECK(future != nullptr && future->newer_than_build);
    CHECK(future->file_format_version == kFormatVersion + 1);
    const TemplateInfo* later = find(list, "Band", "Later");
    CHECK(later != nullptr && later->newer_than_build);
    return 0;
}

int test_junk_is_left_out_and_not_a_category() {
    MapFolder f;
    f.add_dirs("/t/Broken");
    f.files["/t/Broken/Broken.sonora"] = "not a session";
    // Under a folder whose file is junk: not a category either.
    f.add_template("/t/Broken", "Under", kFormatVersion);
    f.add_template("/t", "Good", kFormatVersion);
    const std::vector<TemplateInfo> list = list_templates(f, "/t");
    CHECK(list.size() == 1);
    CHECK(list[0].name == "Good");
    return 0;
}

}  // namespace

int main() {
    int failures = 0;
    failures += test_categories_one_level();
    failures += test_a_template_is_never_a_category();
    failures += test_newer_than_build_is_listed_and_flagged();
    failures += test_junk_is_left_out_and_not_a_category();
    if (failures == 0) std::printf("session template list tests passed\n");
    return failures == 0 ? 0 : 1;
}
