// SPDX-License-Identifier: GPL-3.0-or-later
//
// The template browser's entry (ADR-0146): a TemplateInfo built the old way
// still compiles and reads as uncategorised and current; the version is
// read from the file's head without a load, so a template from a newer
// build is flagged rather than refused, and bytes that are no session file
// leave the entry untouched.
#include <cstdint>
#include <string>

#include "model_test.h"
#include "sonora/model/session_file.h"
#include "sonora/model/session_template.h"

using namespace model_test;

namespace {

std::string header_only(std::int64_t version) {
    // Nothing a full load would accept: only the key the version read needs.
    return "{\"sonora_session\": " + std::to_string(version) + ", \"not\": \"a session\"}";
}

int test_defaults_keep_old_callers() {
    TemplateInfo info;
    info.name = "Tracking";
    info.file = "/t/Tracking/Tracking.sonora";
    CHECK(info.category.empty());
    CHECK(info.file_format_version == 0);
    CHECK(!info.newer_than_build);

    const TemplateInfo braced{"Mix", "/t/Mix/Mix.sonora"};
    CHECK(braced.category.empty());
    CHECK(!braced.newer_than_build);
    return 0;
}

int test_category_is_a_plain_field() {
    TemplateInfo info{"Drums", "/t/Band/Drums/Drums.sonora", "Band"};
    CHECK(info.category == "Band");
    return 0;
}

int test_boundary() {
    CHECK(!format_newer_than_build(kFormatVersion));
    CHECK(!format_newer_than_build(1));
    CHECK(format_newer_than_build(kFormatVersion + 1));
    return 0;
}

int test_current_template_is_not_newer() {
    const Snapshot tmpl = template_of(new_session(settings()));
    std::string bytes;
    std::string why;
    CHECK(serialize(tmpl, WriteInfo{}, bytes, why));
    TemplateInfo info{"Empty", "/t/Empty/Empty.sonora"};
    CHECK(read_template_version(bytes, info));
    CHECK(info.file_format_version == kFormatVersion);
    CHECK(!info.newer_than_build);
    return 0;
}

int test_newer_is_flagged_without_a_load() {
    TemplateInfo info{"Future", "/t/Future/Future.sonora"};
    CHECK(read_template_version(header_only(kFormatVersion + 1), info));
    CHECK(info.file_format_version == kFormatVersion + 1);
    CHECK(info.newer_than_build);

    TemplateInfo old{"Old", "/t/Old/Old.sonora"};
    CHECK(read_template_version(header_only(18), old));
    CHECK(old.file_format_version == 18);
    CHECK(!old.newer_than_build);
    return 0;
}

int test_not_a_session_file_leaves_info() {
    TemplateInfo info{"Junk", "/t/Junk/Junk.sonora", "Band"};
    CHECK(!read_template_version("not json", info));
    CHECK(!read_template_version("{\"other\": 3}", info));
    CHECK(info.file_format_version == 0);
    CHECK(!info.newer_than_build);
    CHECK(info.category == "Band");
    return 0;
}

}  // namespace

int main() {
    int failures = 0;
    failures += test_defaults_keep_old_callers();
    failures += test_category_is_a_plain_field();
    failures += test_boundary();
    failures += test_current_template_is_not_newer();
    failures += test_newer_is_flagged_without_a_load();
    failures += test_not_a_session_file_leaves_info();
    if (failures == 0) std::printf("session template info tests passed\n");
    return failures == 0 ? 0 : 1;
}
