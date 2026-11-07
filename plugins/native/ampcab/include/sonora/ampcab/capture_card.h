// SPDX-License-Identifier: GPL-3.0-or-later
//
// The capture card (ADR-0069, from ADR-0011 §4.6): the provenance record
// that must exist, in `brain/models/<id>.md`, before an amp capture or a
// cabinet IR we ship may enter `plugins/native/ampcab/captures/manifest.json`.
// The machine part of a card is ONE fenced ```card-json block; the prose
// around it is free.
//
// Build-time and load-time code only: everything here allocates and none of
// it is reachable from a `process`.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sonora::ampcab {

// ADR-0011 §4.6 bounds, in decimal megabytes (the smaller reading of "MB").
// The 100 MB total covers both manifests (this one and
// plugins/native/models/manifest.json); until they merge each manifest is
// held to it on its own (ADR-0069 §5).
constexpr std::uint64_t kMaxCaptureFileBytes = 25'000'000;
constexpr std::uint64_t kMaxManifestTotalBytes = 100'000'000;

enum class CardKind { capture, ir };
enum class Ownership { owned, rented };
// What was captured. Only hardware is allowed; `plugin` is parsed so that a
// card claiming it is refused by name, never mistaken for a typo (ADR-0011
// §4.6: capturing a commercial amp plugin is black-box cloning).
enum class CaptureSource { hardware, plugin };

struct CaptureUnit {
    std::string type;    // the real unit, for provenance; not a shipped name
    std::string serial;
    Ownership ownership = Ownership::owned;
    // Required in the card when rented; written only then.
    bool rental_terms_allow_recording = false;
};

// A licensed third-party capture or IR. `present` false means our own work
// (the card's `third_party` is null or absent).
struct ThirdParty {
    bool present = false;
    std::string licence_path;  // empty: no licence recorded (refused)
};

// Present when kind == ir.
struct IrDetails {
    std::string cabinet;
    std::string speaker;
    std::string microphone;
    std::string position;
    std::string room;
};

// Present when kind == capture.
struct CaptureDetails {
    std::string architecture;  // e.g. "wavenet-standard"
    std::uint32_t sample_rate = 0;
    bool has_esr_held_out = false;
    double esr_held_out = 0.0;  // error-to-signal ratio on held-out audio
    std::string train_commit;
};

struct CardFile {
    std::string name;    // shipped file name, e.g. "crunch-head.nam"
    std::string sha256;  // 64 lower-case hex digits
    std::uint64_t size = 0;
};

struct CaptureCard {
    std::string id;    // ^[a-z0-9-]+$, also the card's file stem
    std::string name;  // the shipped, user-facing name: named by its sound
    CardKind kind = CardKind::capture;
    CaptureSource source = CaptureSource::hardware;
    CaptureUnit unit;
    // Control name -> setting, in card order. Numbers keep their lexeme.
    std::vector<std::pair<std::string, std::string>> settings;
    std::string rig;
    std::string date;  // YYYY-MM-DD
    ThirdParty third_party;
    IrDetails ir;
    CaptureDetails capture;
    CardFile file;
};

bool operator==(const CaptureCard& a, const CaptureCard& b);
inline bool operator!=(const CaptureCard& a, const CaptureCard& b) {
    return !(a == b);
}

// Reads the single ```card-json block of a card's Markdown into `out`.
// Returns the refusals; empty means `out` is complete. Never throws. A
// refusal here is about shape (no block, two blocks, bad JSON, a missing or
// mistyped field, an unknown enum value); policy is validate_card's.
std::vector<std::string> parse_card(std::string_view md, CaptureCard& out);

// The deny list's text (plugins/native/ampcab/tools/ampcab_name_deny.txt):
// one term per line, `#` starts a comment, blank lines ignored. Terms are
// returned normalised (see validate_card).
std::vector<std::string> parse_name_deny(std::string_view text);

// The policy of ADR-0011 §4.6 and ADR-0069 §3. Returns every refusal, never
// throws. `deny` holds normalised terms (parse_name_deny); a shipped name
// (id, name, file.name) is refused when its lower-case alphanumerics contain
// a term's, so "jcm-800" and "JCM800" match alike. unit.type is not
// checked: the card must name the real unit it records.
std::vector<std::string> validate_card(const CaptureCard& card,
                                       const std::vector<std::string>& deny);

// The capture file against its card: size and SHA-256. Refusals, never
// throws. The build check runs it before a capture enters the image.
std::vector<std::string> check_capture_file(const CaptureCard& card,
                                            std::string_view bytes);

// The card back as Markdown with one ```card-json block, fields in the
// order ADR-0069 lists them. parse_card(format_card(c)) yields c.
std::string format_card(const CaptureCard& card);

}  // namespace sonora::ampcab
