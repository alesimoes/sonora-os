// SPDX-License-Identifier: GPL-3.0-or-later
//
// The capture card contract (ADR-0069): the fakes survive format_card and
// parse_card unchanged and pass validate_card; each policy refusal of §3
// and each shape refusal of parse_card fires on the one field it names.
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "sonora/ampcab/capture_card.h"
#include "sonora/ampcab/capture_card_fake.h"

namespace {

using namespace sonora::ampcab;

int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, \
                         #cond);                                        \
            ++failures;                                                 \
        }                                                               \
    } while (0)

const std::vector<std::string> kDeny =
    parse_name_deny("# brands and artists, one per line\n"
                    "Acme Amps\n"
                    "ZX-900   # a model number\n"
                    "\n"
                    "famousguitarist\n");

bool mentions(const std::vector<std::string>& refusals, const std::string& word) {
    for (const auto& r : refusals)
        if (r.find(word) != std::string::npos) return true;
    return false;
}

void dump(const char* what, const std::vector<std::string>& refusals) {
    for (const auto& r : refusals) std::fprintf(stderr, "  %s: %s\n", what, r.c_str());
}

void round_trip(const CaptureCard& card) {
    const std::string md = format_card(card);
    CaptureCard back;
    const auto refusals = parse_card(md, back);
    dump("parse", refusals);
    CHECK(refusals.empty());
    CHECK(back == card);
    const auto policy = validate_card(back, kDeny);
    dump("validate", policy);
    CHECK(policy.empty());
}

// One field broken, exactly one refusal, naming `word`.
void refuses(const char* label, const std::function<void(CaptureCard&)>& breaker,
             const CaptureCard& base, const std::string& word) {
    CaptureCard c = base;
    breaker(c);
    const auto r = validate_card(c, kDeny);
    if (r.size() != 1 || !mentions(r, word)) {
        std::fprintf(stderr, "refusal case \"%s\" (want one mentioning %s):\n", label,
                     word.c_str());
        dump("got", r);
        ++failures;
    }
}

void test_deny_list_parsing() {
    CHECK(kDeny.size() == 3);
    CHECK(kDeny[0] == "acmeamps");
    CHECK(kDeny[1] == "zx900");
    CHECK(kDeny[2] == "famousguitarist");
}

void test_policy_refusals() {
    const CaptureCard cap = valid_capture_card();
    const CaptureCard ir = valid_ir_card();
    refuses("unowned unit", [](CaptureCard& c) { c.unit.ownership = Ownership::rented; },
            cap, "rental_terms_allow_recording");
    refuses("rented without terms",
            [](CaptureCard& c) { c.unit.rental_terms_allow_recording = false; }, ir,
            "rental_terms_allow_recording");
    refuses("plugin source", [](CaptureCard& c) { c.source = CaptureSource::plugin; },
            cap, "plugin");
    refuses("third party without licence", [](CaptureCard& c) {
        c.third_party.present = true;
    }, ir, "licence_path");
    refuses("sha256 short", [](CaptureCard& c) { c.file.sha256.pop_back(); }, cap,
            "sha256");
    refuses("sha256 upper case", [](CaptureCard& c) { c.file.sha256[0] = 'E'; }, cap,
            "sha256");
    refuses("size over 25 MB",
            [](CaptureCard& c) { c.file.size = kMaxCaptureFileBytes + 1; }, cap,
            "file.size");
    refuses("missing esr", [](CaptureCard& c) { c.capture.has_esr_held_out = false; },
            cap, "esr_held_out");
    refuses("bad id", [](CaptureCard& c) { c.id = "Crunch_Head"; }, cap, "id");
    refuses("bad date", [](CaptureCard& c) { c.date = "2026-13-01"; }, cap, "date");
    refuses("brand in name", [](CaptureCard& c) { c.name = "ACME-amps head"; }, cap,
            "acmeamps");
    refuses("model number in id", [](CaptureCard& c) { c.id = "zx-900-crunch"; }, cap,
            "zx900");
    refuses("artist in file name",
            [](CaptureCard& c) { c.file.name = "Famous Guitarist.wav"; }, ir,
            "famousguitarist");

    // A licensed third-party IR is allowed; the unit type, being provenance,
    // may name the real hardware without tripping the deny list.
    CaptureCard ok = ir;
    ok.third_party.present = true;
    ok.third_party.licence_path = "brain/models/licences/ir-pack.txt";
    ok.unit.type = "Acme Amps ZX-900 cabinet";
    CHECK(validate_card(ok, kDeny).empty());
    round_trip(ok);
}

void test_file_check() {
    const CaptureCard cap = valid_capture_card();
    CHECK(check_capture_file(cap, "").empty());
    const auto r = check_capture_file(cap, "x");
    CHECK(r.size() == 2);
}

std::vector<std::string> parse(const std::string& md) {
    CaptureCard c;
    return parse_card(md, c);
}

// The block of the capture fake with one substring replaced.
std::string edited(const std::string& from, const std::string& to) {
    std::string md = format_card(valid_capture_card());
    const auto at = md.find(from);
    CHECK(at != std::string::npos);
    if (at != std::string::npos) md.replace(at, from.size(), to);
    return md;
}

void test_parse_refusals() {
    const std::string good = format_card(valid_capture_card());
    CHECK(parse("prose only, no block\n").size() == 1);
    CHECK(mentions(parse("text\n```card-json\n{}\n"), "not closed"));
    CHECK(mentions(parse(good + "\n" + good), "exactly one"));
    CHECK(mentions(parse("```card-json\n{ \"id\": \n```\n"), "line"));
    CHECK(mentions(parse("```card-json\n[]\n```\n"), "one object"));
    CHECK(mentions(parse(edited("\"esr_held_out\"", "\"esr_heldout\"")),
                   "not a card field"));
    CHECK(mentions(parse(edited("\"owned\"", "\"borrowed\"")), "owned|rented"));
    CHECK(mentions(parse(edited("\"sample_rate\": 48000", "\"sample_rate\": \"48k\"")),
                   "sample_rate"));
    CHECK(mentions(parse(edited("\"capture\": {", "\"ir\": {}, \"capture\": {")),
                   "ir is not a field"));
    CHECK(mentions(parse(edited("\"rig\"", "\"rigg\"")), "rig is missing"));

    // Prose around the block is free, an inline mention of the tag is not a
    // fence, and CRLF line ends read the same.
    std::string crlf;
    for (const char ch : good) {
        if (ch == '\n') crlf += '\r';
        crlf += ch;
    }
    CaptureCard back;
    CHECK(parse_card("Intro naming ```card-json inline.\r\n\r\n" + crlf +
                         "\r\nNotes after.\r\n",
                     back)
              .empty());
    CHECK(back == valid_capture_card());

    // A missing esr parses (shape is fine) and is refused by policy.
    CaptureCard no_esr;
    CHECK(parse_card(edited("\"esr_held_out\": 0.0125,", ""), no_esr).empty());
    CHECK(!no_esr.capture.has_esr_held_out);
    CHECK(mentions(validate_card(no_esr, kDeny), "esr_held_out"));
}

}  // namespace

int main() {
    round_trip(valid_capture_card());
    round_trip(valid_ir_card());
    test_deny_list_parsing();
    test_policy_refusals();
    test_file_check();
    test_parse_refusals();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("capture card contract: ok");
    return EXIT_SUCCESS;
}
