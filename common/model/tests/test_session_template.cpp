// SPDX-License-Identifier: GPL-3.0-or-later
//
// Session templates (ADR-0030, daw.model.session_mgmt): what a template
// keeps of a session (tracks, buses, routing, inserts with their values and
// state, automation, the tempo map, the transport, markers on request) and
// what it drops (every region, every source, the left takes, the session's
// identity); that it is a session file of the current format, read with
// the same version checks; and that a session made from one is a new
// session with its own id, never the template's material.
#include <memory>
#include <string>

#include "model_test.h"
#include "sonora/model/session_file.h"
#include "sonora/model/session_template.h"

using namespace model_test;

namespace {

constexpr const char* kSha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

struct Rich {
    History h{new_session(settings())};
    Id vox = 0, vox_main = 0, vox_alt = 0, synth = 0, synth_playlist = 0;
    Id verb = 0, send = 0, insert = 0, region = 0, midi_region = 0, marker = 0;

    // A session as a tracking day leaves it: two tracks with material on
    // them, a bus, a send, an insert with its state, automation, a marker,
    // a tempo change, a left take and an identity.
    bool build() {
        Ids ids(h.current());
        std::string why;
        const AddTrack t = add_track(ids, "Vox", 2);
        vox = t.track;
        vox_main = t.playlist;
        if (!h.apply(t, why)) return fail(why);
        const AddSource src = add_source(ids, 480000, 2, vox);
        if (!h.apply(src, why)) return fail(why);
        region = ids();
        if (!h.apply(PlaceRegion{vox_main, model_test::region(region, src.source.id, 48000, 96000, 0, 2), 0},
                     why)) {
            return fail(why);
        }
        vox_alt = ids();
        if (!h.apply(AddPlaylist{vox, vox_alt, "Vox.02"}, why)) return fail(why);
        const Id alt_region = ids();
        if (!h.apply(PlaceRegion{vox_alt, model_test::region(alt_region, src.source.id, 0, 48000, 0, 2), 0},
                     why)) {
            return fail(why);
        }
        if (!h.apply(SetActivePlaylist{vox, vox_alt}, why)) return fail(why);

        AddBus bus;
        bus.bus = verb = ids();
        bus.name = "Verb";
        bus.mix.gain_db = -3.0f;
        if (!h.apply(bus, why)) return fail(why);
        AddSend s;
        s.strip = vox;
        s.send = send = ids();
        s.slot = 2;
        s.bus = verb;
        s.mix.level_db = -12.0f;
        s.mix.pre = true;
        if (!h.apply(s, why)) return fail(why);
        if (!h.apply(SetOutput{vox, verb}, why)) return fail(why);

        AddInsert in;
        in.track = vox;
        in.insert = insert = ids();
        in.slot = 3;
        in.plugin = PluginRef{PluginFormat::Lv2, "urn:test#comp", "Comp", "1.2"};
        in.params = {ParamValue{"ratio", 4.0f}, ParamValue{"threshold", -18.0f}};
        in.state = StateRef{plugin_state_file(kSha), kSha, 321};
        if (!h.apply(in, why)) return fail(why);
        if (!h.apply(SetInsertBypass{insert, true}, why)) return fail(why);

        EditAutomation lane;
        lane.track = vox;
        lane.param = AutomationParam::Volume;
        lane.from = 0;
        lane.to = 960000;
        lane.points = {AutomationPoint{0, 0.5f}, AutomationPoint{480000, 0.25f}};
        if (!h.apply(lane, why)) return fail(why);

        TrackMix m = *h.current().mix.find(vox);
        m.gain_db = -4.5f;
        m.pan = 0.25f;
        m.solo_safe = true;
        if (!h.apply(SetMix{m}, why)) return fail(why);

        AddTrack mt = add_track(ids, "Synth", 0);
        mt.kind = TrackKind::Midi;
        mt.mix.input.clear();
        synth = mt.track;
        synth_playlist = mt.playlist;
        if (!h.apply(mt, why)) return fail(why);
        AddMidiSource ms;
        ms.source.id = ids();
        ms.source.path = "MIDI Files/take_1.mid";
        ms.source.length = 48000;
        ms.source.events = 1;
        ms.source.take = 1;
        ms.source.recorded_on_track = synth;
        if (!h.apply(ms, why)) return fail(why);
        MidiLists seq;
        seq.events.push_back(MidiEvent{0, 0xB0, 7, 100});
        MidiRegion r;
        r.id = midi_region = ids();
        r.name = "Synth_01";
        r.length = kTicksPerQuarter;
        r.source = ms.source.id;
        r.data = std::make_shared<const MidiSequence>(seq);
        if (!h.apply(PlaceMidiRegion{synth_playlist, r, 0, {}}, why)) return fail(why);

        marker = ids();
        if (!h.apply(AddMarker{Marker{marker, 96000, "Chorus", 0}}, why)) return fail(why);

        SessionSettings st = h.current().settings;
        st.tempo = {TempoEntry{0, 92.0f, {{6, 8}}}, TempoEntry{960000, 120.0f, {{4, 4}}}};
        st.transport.loop = {{48000, 96000}};
        st.transport.count_off_bars = 1;
        st.session_id = "6f1c2b0e-9a1d-4c55-8e2a-0d5f3b7c9e11";
        st.left_takes = {"Audio Files/Vox_03.wav"};
        if (!h.apply(SetSettings{st}, why)) return fail(why);
        return invariants_hold(h.current());
    }
    static bool fail(const std::string& why) {
        std::fprintf(stderr, "  apply refused: %s\n", why.c_str());
        return false;
    }
};

const TrackEdit* track(const Snapshot& s, Id id) {
    for (const auto& t : s.edit->tracks)
        if (t->id == id) return t.get();
    return nullptr;
}

// Tracks, buses and routing, inserts and their values, automation, the
// tempo map and the transport come through; the material does not.
int test_template_keeps_the_setup_and_drops_the_material() {
    Rich r;
    CHECK(r.build());
    const Snapshot& s = r.h.current();
    const Snapshot t = template_of(s);
    CHECK(invariants_hold(t));

    CHECK(t.edit->tracks.size() == 2);
    const TrackEdit* vox = track(t, r.vox);
    const TrackEdit* was = track(s, r.vox);
    CHECK(vox != nullptr && was != nullptr);
    CHECK(vox->name == "Vox" && vox->channels == 2 && vox->kind == TrackKind::Audio);
    // The playlist the session plays, emptied; the others are takes.
    CHECK(vox->playlists.size() == 1);
    CHECK(vox->active_playlist == r.vox_alt && vox->playlists[0]->id == r.vox_alt);
    CHECK(vox->playlists[0]->name == "Vox.02");
    CHECK(vox->playlists[0]->regions.empty() && vox->playlists[0]->crossfades.empty());
    CHECK(vox->inserts == was->inserts && vox->sends == was->sends && vox->output == r.verb);
    CHECK(vox->automation.size() == 1 && *vox->automation[0] == *was->automation[0]);

    const TrackEdit* synth = track(t, r.synth);
    CHECK(synth != nullptr && synth->kind == TrackKind::Midi);
    CHECK(synth->midi_playlists.size() == 1 && synth->midi_playlists[0]->regions.empty());
    CHECK(synth->active_playlist == r.synth_playlist);

    CHECK(t.edit->buses == s.edit->buses);
    CHECK(*t.mix.find(r.vox) == *s.mix.find(r.vox));
    CHECK(*t.mix.find(r.synth) == *s.mix.find(r.synth));
    const InsertMix* im = t.mix.find_insert(r.insert);
    CHECK(im != nullptr && *im == *s.mix.find_insert(r.insert));
    CHECK(im->bypass && im->state && im->state->sha256 == kSha && im->params.size() == 2);
    CHECK(*t.mix.find_send(r.send) == *s.mix.find_send(r.send));
    CHECK(*t.mix.find_bus(r.verb) == *s.mix.find_bus(r.verb));
    CHECK(t.mix.bus == s.mix.bus);

    CHECK(t.sources->sources.empty() && t.sources->midi_sources.empty());
    CHECK(t.edit->markers->markers.empty());
    CHECK(t.settings.tempo == s.settings.tempo);
    CHECK(t.settings.timecode == s.settings.timecode);
    CHECK(t.settings.transport == s.settings.transport);
    CHECK(t.settings.sample_rate == s.settings.sample_rate);
    CHECK(t.settings.audio_format == s.settings.audio_format);
    CHECK(t.settings.name == s.settings.name);
    // Not this session's any more: no identity and no take of its own.
    CHECK(t.settings.session_id.empty() && t.settings.left_takes.empty());
    CHECK(t.revision == 0);
    // Ids stay unique against everything the session had.
    CHECK(t.next_id == s.next_id);

    // The session itself is as it was.
    CHECK(!track(s, r.vox)->playlists.empty() && s.sources->sources.size() == 1);
    return 0;
}

int test_markers_on_request() {
    Rich r;
    CHECK(r.build());
    TemplateOptions with;
    with.markers = true;
    const Snapshot t = template_of(r.h.current(), with);
    CHECK(invariants_hold(t));
    CHECK(t.edit->markers->markers.size() == 1 && t.edit->markers->markers[0].name == "Chorus");
    return 0;
}

// A template is a session file of this format: no key of its own, no
// version of its own, so it opens through deserialize() with every check
// a session file has, and a newer one is refused the same way.
int test_template_is_a_session_file() {
    Rich r;
    CHECK(r.build());
    const Snapshot t = template_of(r.h.current());
    std::string bytes, error;
    CHECK(serialize(t, WriteInfo{"sonora-ui", 1}, bytes, error));
    CHECK(bytes.find("Audio Files/") == std::string::npos);
    CHECK(bytes.find("MIDI Sequences/") == std::string::npos);
    CHECK(bytes.find("Plugin State/" + std::string(kSha) + ".state") != std::string::npos);
    std::int64_t version = 0;
    CHECK(file_format_version(bytes, version) && version == kFormatVersion);

    Loaded loaded;
    LoadError le;
    CHECK(deserialize(bytes, loaded, le));
    CHECK(loaded.repairs.empty() && loaded.missing_sequences.empty());
    CHECK(same_document(loaded.snapshot, t));

    const std::string current = "\"sonora_session\": " + std::to_string(kFormatVersion);
    const std::size_t at = bytes.find(current);
    CHECK(at != std::string::npos);
    std::string newer = bytes;
    newer.replace(at, current.size(), "\"sonora_session\": " + std::to_string(kFormatVersion + 1));
    Loaded refused;
    CHECK(!deserialize(newer, refused, le));
    CHECK(mentions(le.text(), std::to_string(kFormatVersion + 1).c_str()));
    return 0;
}

// Saving a template of a template changes nothing: what Save as Template
// writes from a session made from one is that template again.
int test_template_of_a_template() {
    Rich r;
    CHECK(r.build());
    const Snapshot once = template_of(r.h.current());
    const Snapshot twice = template_of(once);
    CHECK(same_document(once, twice));
    return 0;
}

// New from Template: a new session with its own name and id, the setup of
// the template, and none of a session's material even when the file given
// as a template still holds some (a session file copied there by hand).
int test_session_from_template() {
    Rich r;
    CHECK(r.build());
    TemplateOptions with;
    with.markers = true;
    const Snapshot t = template_of(r.h.current(), with);
    const std::string id = new_session_id();
    const Snapshot s = session_from_template(t, "Song", id);
    CHECK(invariants_hold(s));
    CHECK(s.settings.name == "Song" && s.settings.session_id == id);
    CHECK(s.settings.tempo == t.settings.tempo);
    CHECK(s.edit->tracks.size() == 2 && s.edit->buses == t.edit->buses);
    CHECK(s.edit->markers->markers.size() == 1);
    CHECK(*s.mix.find_insert(r.insert) == *t.mix.find_insert(r.insert));
    CHECK(s.revision == 0 && s.next_id == t.next_id);

    const Snapshot from_session = session_from_template(r.h.current(), "Other", new_session_id());
    CHECK(invariants_hold(from_session));
    CHECK(from_session.sources->sources.empty() && from_session.sources->midi_sources.empty());
    CHECK(from_session.settings.left_takes.empty());
    for (const auto& tr : from_session.edit->tracks) {
        for (const auto& p : tr->playlists) CHECK(p->regions.empty());
        for (const auto& p : tr->midi_playlists) CHECK(p->regions.empty());
    }
    // Its markers stay: a template's markers are the template's choice.
    CHECK(from_session.edit->markers->markers.size() == 1);
    return 0;
}

}  // namespace

int main() {
    int failures = 0;
    failures += test_template_keeps_the_setup_and_drops_the_material();
    failures += test_markers_on_request();
    failures += test_template_is_a_session_file();
    failures += test_template_of_a_template();
    failures += test_session_from_template();
    if (failures == 0) std::printf("session template tests passed\n");
    return failures == 0 ? 0 : 1;
}
