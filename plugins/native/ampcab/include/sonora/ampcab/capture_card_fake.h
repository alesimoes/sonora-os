// SPDX-License-Identifier: GPL-3.0-or-later
//
// Valid capture cards for tests (ADR-0069). Each survives format_card then
// parse_card unchanged and passes validate_card with any deny list that does
// not name its own words; a test breaks one field to see one refusal.
#pragma once

#include "sonora/ampcab/capture_card.h"

namespace sonora::ampcab {

// sha256_hex("") so a test can check an empty file against either card.
constexpr const char* kEmptySha256 =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

// An owned head, captured by our rig, WaveNet standard at 48 kHz.
inline CaptureCard valid_capture_card() {
    CaptureCard c;
    c.id = "crunch-head-el34";
    c.name = "EL34 head, crunch";
    c.kind = CardKind::capture;
    c.source = CaptureSource::hardware;
    c.unit.type = "two-channel EL34 head, 50 W";
    c.unit.serial = "SN-0001";
    c.unit.ownership = Ownership::owned;
    c.settings = {{"gain", "6"}, {"bass", "5"}, {"middle", "7"},
                  {"treble", "6.5"}, {"channel", "lead"}};
    c.rig = "reamp box, load box, interface at 48 kHz";
    c.date = "2026-11-01";
    c.capture.architecture = "wavenet-standard";
    c.capture.sample_rate = 48000;
    c.capture.has_esr_held_out = true;
    c.capture.esr_held_out = 0.0125;
    c.capture.train_commit = "0123456789abcdef0123456789abcdef01234567";
    c.file.name = "crunch-head-el34.nam";
    c.file.sha256 = kEmptySha256;
    c.file.size = 0;
    return c;
}

// A rented cabinet whose rental terms allow recording, one mic position.
inline CaptureCard valid_ir_card() {
    CaptureCard c;
    c.id = "4x12-closed-dynamic-cap-edge";
    c.name = "4x12 closed-back, dynamic mic, cap edge";
    c.kind = CardKind::ir;
    c.source = CaptureSource::hardware;
    c.unit.type = "4x12 closed-back cabinet";
    c.unit.serial = "CAB-17";
    c.unit.ownership = Ownership::rented;
    c.unit.rental_terms_allow_recording = true;
    c.rig = "sweep through power amp, 48 kHz, deconvolved";
    c.date = "2026-10-30";
    c.ir.cabinet = "4x12 closed-back";
    c.ir.speaker = "12 inch, 25 W, ceramic magnet";
    c.ir.microphone = "dynamic cardioid";
    c.ir.position = "cap edge, 2 cm, on axis";
    c.ir.room = "treated live room, 4 x 5 m";
    c.file.name = "4x12-closed-dynamic-cap-edge.wav";
    c.file.sha256 = kEmptySha256;
    c.file.size = 0;
    return c;
}

}  // namespace sonora::ampcab
