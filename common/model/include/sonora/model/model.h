// SPDX-License-Identifier: GPL-3.0-or-later
//
// The session document (brain/specs/timeline-model.md, ADR-0009).
//
// Never on the audio thread (ADR-0007 §2): these types allocate, share
// ownership and hold strings. The audio thread sees only the plan that
// engine/control builds from a Snapshot. Standard C++17 only (ADR-0008): no
// file, no clock, no thread.
//
// Everything reachable from a Snapshot is immutable once built. An edit
// builds a new EditState that shares every track and playlist it did not
// touch with the one before, which is what makes undo a pointer and lets a
// saver or a plan builder read a snapshot while the next edit is applied.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sonora/midi/notes.h"

namespace sonora::model {

using Id = std::uint64_t;      // one counter for every object; 0 is "none"
using Frames = std::int64_t;   // samples at the session rate, never negative
// MIDI positions, lengths and offsets (ADR-0014 §2). The same type as
// Frames: what keeps the two apart is a playlist's Timebase, which apply()
// checks, not the compiler.
using Ticks = std::int64_t;

// Pro Tools' published internal resolution. What decides it is that a tick
// is shorter than a frame above 3 bpm at 48 kHz, so a recorded event comes
// back to its frame (midi.md §1).
constexpr Ticks kTicksPerQuarter = 960'000;

// Integers in the session file stay inside what jq, Python and JavaScript
// read exactly (session-format.md, "Numbers").
constexpr std::int64_t kMaxInteger = (std::int64_t{1} << 53) - 1;

enum class FadeShape : std::uint8_t { Linear, EqualPower, SCurve };
enum class Align : std::uint8_t { Centre, Pre, Post };
enum class AudioFormat : std::uint8_t { Int24, Float32 };
enum class SourceKind : std::uint8_t { Recorded, Imported };
enum class MonitorMode : std::uint8_t { Auto, Input, Off };
enum class Fps : std::uint8_t { F23_976, F24, F25, F29_97, F29_97df, F30 };
// Audio plays the track's own regions; Midi sends its regions to an
// external port; Instrument plays them through a hosted instrument
// (ADR-0014 §1).
enum class TrackKind : std::uint8_t { Audio, Midi, Instrument };
// A playlist's unit: frames for audio, ticks for MIDI. Every region
// operation that carries a position says which it means, so a sender that
// got it wrong is refused instead of moving a region by about 25 times.
enum class Timebase : std::uint8_t { Frames, Ticks };

struct SourceRef {
    Id source = 0;
    std::uint16_t channel = 0;
    bool operator==(const SourceRef& o) const {
        return source == o.source && channel == o.channel;
    }
};

struct Fade {
    Frames length = 0;
    FadeShape shape = FadeShape::Linear;
    bool operator==(const Fade& o) const {
        return length == o.length && shape == o.shape;
    }
};

// Every gain the document holds (track, bus, region), in dB. The audio
// thread multiplies by 10^(dB/20) with no check of its own, so the range is
// what keeps inf (and the NaN that inf times silence makes) and subnormals
// out of it: the ends are about 15.8 and 6.3e-8.
inline constexpr float kMinGainDb = -144.0f;
inline constexpr float kMaxGainDb = 24.0f;
// The monitor bus's pan law at centre, in dB: 0 (balance) down to this. A
// positive law is a negative exponent, which is inf at a hard pan.
inline constexpr float kMinPanLawDb = -12.0f;

struct Region {
    Id id = 0;
    std::string name;
    Frames position = 0;       // timeline frame of the region's first frame
    Frames length = 0;         // > 0
    Frames source_offset = 0;  // the source frame that plays at `position`
    std::vector<SourceRef> channels;  // one per track channel
    float gain_db = 0.0f;
    bool mute = false;
    Fade fade_in, fade_out;

    Frames end() const { return position + length; }
    bool operator==(const Region& o) const;
};

// Joins two regions that butt on one playlist, drawing on each one's handle
// (session-format.md, "Crossfades"). Named by its two regions; it has no id.
struct Crossfade {
    Id left = 0;
    Id right = 0;
    Frames length = 0;
    Align align = Align::Centre;
    FadeShape shape = FadeShape::EqualPower;
    bool operator==(const Crossfade& o) const {
        return left == o.left && right == o.right && length == o.length &&
               align == o.align && shape == o.shape;
    }
};

// The span a crossfade covers either side of the cut: it runs over
// [cut - before, cut + after). An odd centred length puts the extra frame
// after the cut.
void crossfade_extent(const Crossfade& x, Frames& before, Frames& after);

struct Playlist {
    Id id = 0;
    std::string name;
    std::vector<Region> regions;         // sorted by position, never overlapping
    std::vector<Crossfade> crossfades;   // sorted by the left region's position
    bool operator==(const Playlist& o) const;
};

// --- MIDI regions (ADR-0014 §1, brain/specs/midi.md §3) --------------------

// The largest SysEx message a region stores, F0 to F7. Larger ones are not
// stored: import reports each one, never truncates it (midi.md §4.4).
inline constexpr std::size_t kMaxSysexBytes = 4096;

// A channel message other than a note on or off: status 0xA0 to 0xEF, data
// bytes 0 to 127, data2 0 for the two-byte messages (program change,
// channel pressure).
struct MidiEvent {
    Ticks tick = 0;
    std::uint8_t status = 0xB0;
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;
    bool operator==(const MidiEvent& o) const {
        return tick == o.tick && status == o.status && data1 == o.data1 && data2 == o.data2;
    }
};

struct MidiSysEx {
    Ticks tick = 0;
    std::vector<std::uint8_t> bytes;   // the whole message, 0xF0 to 0xF7
    bool operator==(const MidiSysEx& o) const { return tick == o.tick && bytes == o.bytes; }
};

// A sequence's file beside the session, "MIDI Sequences/<sha256>.msq"
// (ADR-0015 §3): what a MIDI region's "data" key holds. Named by the hash
// of its bytes, so regions and saves that hold one sequence share one file.
struct SequenceFileRef {
    std::string file;     // midi_sequence_file(sha256)
    std::string sha256;   // 64 lower-case hex digits
    std::uint64_t bytes = 0;
    std::uint64_t notes = 0;
    std::uint64_t events = 0;
    std::uint64_t sysex = 0;
    bool operator==(const SequenceFileRef& o) const {
        return file == o.file && sha256 == o.sha256 && bytes == o.bytes && notes == o.notes &&
               events == o.events && sysex == o.sysex;
    }
};

// "MIDI Sequences/<sha256>.msq"
std::string midi_sequence_file(const std::string& sha256);

// A sequence's contents as three flat lists: what an operation, a sequence
// file and a format-5 session file carry, and what a MidiSequence is built
// from.
struct MidiLists {
    std::vector<midi::Note> notes;   // midi::sort_notes() order
    // By tick; at one tick in stored order, which is meaning (a bank
    // select before its program change).
    std::vector<MidiEvent> events;
    std::vector<MidiSysEx> sysex;    // by tick, at one tick in stored order
    std::uint64_t entries() const { return notes.size() + events.size() + sysex.size(); }
    bool empty() const { return entries() == 0; }
    bool operator==(const MidiLists& o) const {
        return notes == o.notes && events == o.events && sysex == o.sysex;
    }
};

// ADR-0015 §1: a chunk spans four quarter notes of data ticks, not bars, so
// a meter change never re-chunks.
inline constexpr Ticks kMidiChunkTicks = 4 * kTicksPerQuarter;
// The spine has two levels, a leaf per this many chunk indices. One level
// would charge a drag the whole spine, 40 bytes a chunk: 70.3 KiB for a
// 60-minute region at 120 bpm, past the 64 KiB at which ADR-0015
// Unverified 1 calls for a second level.
inline constexpr std::int64_t kMidiLeafChunks = 64;

// The chunk a data tick (a note's start) belongs to.
inline std::int64_t midi_chunk_of(Ticks tick) { return tick / kMidiChunkTicks; }

// What a sequence holds at data ticks [index * kMidiChunkTicks,
// (index + 1) * kMidiChunkTicks), in MidiLists' orders. Never empty inside a
// sequence: an empty chunk is absent. Immutable once shared, by undo steps
// and by the sequences an edit made from one another.
struct MidiChunk {
    std::int64_t index = 0;
    MidiLists data;
};

struct MidiSpineEntry {
    std::shared_ptr<const MidiChunk> chunk;
    // What the chunks before this one in its leaf hold.
    std::uint64_t notes = 0;
    std::uint64_t events = 0;
    std::uint64_t sysex = 0;
};

// The chunks whose index is in [index * kMidiLeafChunks, (index + 1) *
// kMidiLeafChunks), by index. Never empty inside a sequence.
struct MidiSpineLeaf {
    std::int64_t index = 0;
    std::vector<MidiSpineEntry> chunks;
    // What the whole leaf holds.
    std::uint64_t notes = 0;
    std::uint64_t events = 0;
    std::uint64_t sysex = 0;
};

struct MidiSpineNode {
    std::shared_ptr<const MidiSpineLeaf> leaf;
    // What the leaves before this one hold: an index maps to its chunk by
    // two binary searches.
    std::uint64_t notes = 0;
    std::uint64_t events = 0;
    std::uint64_t sysex = 0;
};

// What one region plays, in data ticks (ADR-0015 §1): a spine of canonical
// chunks. Immutable once a region holds it, and shared: by the regions a
// split, duplicate or paste makes, and by every snapshot that did not edit
// it. An edit shares every chunk and leaf it did not touch.
//
// The chunking is a function of the contents alone, never of the edits that
// led there, so the engine's apply and a replica's build the same chunks,
// and equal sequences have equal spines.
class MidiSequence {
public:
    // The notes, events or SysEx messages in order, read through the spine.
    // Random access is two binary searches; iteration walks the chunks.
    template <typename T>
    class List;

    MidiSequence() = default;
    // Each list in MidiLists' order; sequence_problem() of the lists says
    // whether they are. Elements are placed by the chunk of their tick,
    // keeping their order within a chunk.
    explicit MidiSequence(MidiLists lists);
    // From leaves that are canonical: by index, each leaf's chunks by index,
    // inside the leaf's span and none empty. False, with `why`, otherwise.
    static bool from_leaves(std::vector<std::shared_ptr<const MidiSpineLeaf>> leaves,
                            MidiSequence& out, std::string& why);
    // A leaf of `chunks`, which must be by index, in one leaf's span and none
    // empty. False, with `why`, otherwise.
    static bool make_leaf(std::vector<std::shared_ptr<const MidiChunk>> chunks,
                          std::shared_ptr<const MidiSpineLeaf>& out, std::string& why);
    // `base` with the chunks at the indices in `replace` (by index, no index
    // twice) replaced by those: an empty one removes the chunk there. Every
    // other chunk, and every leaf none of them falls in, is shared.
    static MidiSequence replaced(const MidiSequence& base,
                                 const std::vector<std::shared_ptr<const MidiChunk>>& replace);

    std::size_t note_count() const { return static_cast<std::size_t>(notes_); }
    std::size_t event_count() const { return static_cast<std::size_t>(events_); }
    std::size_t sysex_count() const { return static_cast<std::size_t>(sysex_); }
    // Notes, events and SysEx messages: what ADR-0015 §5's budget counts.
    std::uint64_t entries() const { return notes_ + events_ + sysex_; }
    bool empty() const { return entries() == 0; }

    List<midi::Note> notes() const;
    List<MidiEvent> events() const;
    List<MidiSysEx> sysex() const;
    // A flat copy.
    MidiLists lists() const;

    const std::vector<MidiSpineNode>& spine() const { return spine_; }
    // The chunk at `index`, or null.
    const MidiChunk* chunk_at(std::int64_t index) const;

    // Set when the file this sequence was saved in was missing or damaged at
    // open (ADR-0015 owner decision O2): the sequence is empty, its regions
    // are silent and not editable but keep their windows, and a save writes
    // this reference back unchanged so the file can still be restored
    // beside it.
    std::optional<SequenceFileRef> missing;

    bool operator==(const MidiSequence& o) const;

private:
    void count();

    std::vector<MidiSpineNode> spine_;
    std::uint64_t notes_ = 0;
    std::uint64_t events_ = 0;
    std::uint64_t sysex_ = 0;
};

namespace detail {

template <typename T>
struct ChunkList;
template <>
struct ChunkList<midi::Note> {
    static const std::vector<midi::Note>& of(const MidiChunk& c) { return c.data.notes; }
    template <typename N>
    static std::uint64_t before(const N& n) { return n.notes; }
};
template <>
struct ChunkList<MidiEvent> {
    static const std::vector<MidiEvent>& of(const MidiChunk& c) { return c.data.events; }
    template <typename N>
    static std::uint64_t before(const N& n) { return n.events; }
};
template <>
struct ChunkList<MidiSysEx> {
    static const std::vector<MidiSysEx>& of(const MidiChunk& c) { return c.data.sysex; }
    template <typename N>
    static std::uint64_t before(const N& n) { return n.sysex; }
};

}  // namespace detail

template <typename T>
class MidiSequence::List {
    using Of = detail::ChunkList<T>;

public:
    class iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T&;

        iterator() = default;
        const T& operator*() const { return Of::of(*chunk())[e_]; }
        const T* operator->() const { return &**this; }
        iterator& operator++() {
            ++e_;
            settle();
            return *this;
        }
        iterator operator++(int) {
            iterator before = *this;
            ++*this;
            return before;
        }
        bool operator==(const iterator& o) const {
            return leaf_ == o.leaf_ && entry_ == o.entry_ && e_ == o.e_;
        }
        bool operator!=(const iterator& o) const { return !(*this == o); }

    private:
        friend class List;
        iterator(const std::vector<MidiSpineNode>* spine, std::size_t leaf)
            : spine_(spine), leaf_(leaf) {
            settle();
        }
        const MidiChunk* chunk() const {
            return (*spine_)[leaf_].leaf->chunks[entry_].chunk.get();
        }
        // Past the chunks that hold none of T, onto the next element or the end.
        void settle() {
            while (leaf_ < spine_->size()) {
                const MidiSpineLeaf& l = *(*spine_)[leaf_].leaf;
                if (entry_ < l.chunks.size()) {
                    if (e_ < Of::of(*l.chunks[entry_].chunk).size()) return;
                    ++entry_;
                    e_ = 0;
                    continue;
                }
                ++leaf_;
                entry_ = 0;
                e_ = 0;
            }
        }

        const std::vector<MidiSpineNode>* spine_ = nullptr;
        std::size_t leaf_ = 0;
        std::size_t entry_ = 0;
        std::size_t e_ = 0;
    };

    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    iterator begin() const { return iterator(spine_, 0); }
    iterator end() const { return iterator(spine_, spine_->size()); }
    const T& front() const { return (*this)[0]; }
    const T& back() const { return (*this)[size_ - 1]; }

    // `i` below size().
    const T& operator[](std::size_t i) const {
        const std::uint64_t at = i;
        // The first leaf, then chunk, whose elements reach past `at`.
        std::size_t lo = 0, hi = spine_->size();
        while (lo < hi) {
            const std::size_t mid = (lo + hi) / 2;
            const MidiSpineNode& n = (*spine_)[mid];
            if (Of::before(n) + Of::before(*n.leaf) > at) {
                hi = mid;
            } else {
                lo = mid + 1;
            }
        }
        const MidiSpineNode& node = (*spine_)[lo];
        const std::uint64_t in_leaf = at - Of::before(node);
        const std::vector<MidiSpineEntry>& cs = node.leaf->chunks;
        std::size_t a = 0, b = cs.size();
        while (a < b) {
            const std::size_t mid = (a + b) / 2;
            if (Of::before(cs[mid]) + Of::of(*cs[mid].chunk).size() > in_leaf) {
                b = mid;
            } else {
                a = mid + 1;
            }
        }
        return Of::of(*cs[a].chunk)[static_cast<std::size_t>(in_leaf - Of::before(cs[a]))];
    }

    std::vector<T> to_vector() const { return std::vector<T>(begin(), end()); }
    bool operator==(const std::vector<T>& v) const {
        return v.size() == size_ && std::equal(v.begin(), v.end(), begin());
    }
    bool operator!=(const std::vector<T>& v) const { return !(*this == v); }

private:
    friend class MidiSequence;
    List(const std::vector<MidiSpineNode>* spine, std::size_t size)
        : spine_(spine), size_(size) {}

    const std::vector<MidiSpineNode>* spine_;
    std::size_t size_;
};

inline MidiSequence::List<midi::Note> MidiSequence::notes() const {
    return List<midi::Note>(&spine_, note_count());
}
inline MidiSequence::List<MidiEvent> MidiSequence::events() const {
    return List<MidiEvent>(&spine_, event_count());
}
inline MidiSequence::List<MidiSysEx> MidiSequence::sysex() const {
    return List<MidiSysEx>(&spine_, sysex_count());
}

inline std::uint64_t midi_entries(const MidiSequence& s) { return s.entries(); }

// A window onto a sequence, as an audio region is onto its source. A note
// is heard if its start is in [offset, offset + length); notes outside stay
// in `data`, so an undone or dragged-back trim brings them back.
struct MidiRegion {
    Id id = 0;
    std::string name;
    Ticks position = 0;   // timeline tick of the window's start
    Ticks length = 0;     // > 0
    Ticks offset = 0;     // the data tick shown at `position`
    bool mute = false;
    Id source = 0;        // the MIDI source it came from; 0 for none
    std::shared_ptr<const MidiSequence> data;   // never null

    Ticks end() const { return position + length; }
    bool operator==(const MidiRegion& o) const;
};

// The rules of Playlist, in ticks: sorted, never overlapping, overwrite on
// placement. No crossfades and no fades.
struct MidiPlaylist {
    Id id = 0;
    std::string name;
    std::vector<MidiRegion> regions;
    bool operator==(const MidiPlaylist& o) const {
        return id == o.id && name == o.name && regions == o.regions;
    }
};

// --- plugin inserts (ADR-0012 §6, session format version 2) ---------------

// Pro Tools' A to J: the most inserts one chain carries (ADR-0012 §2).
inline constexpr std::uint8_t kInsertSlots = 10;
// The chain cap of ADR-0012 §5.1, so one insert never holds more values
// than its chain can automate.
inline constexpr std::size_t kMaxInsertParams = 8192;

enum class PluginFormat : std::uint8_t { Lv2, Vst3 };

// `id` is the plugin's identity. `name` and `version` are what the scan said
// when the insert was made: shown when the plugin is missing, read by no
// logic.
struct PluginRef {
    PluginFormat format = PluginFormat::Lv2;
    std::string id;   // the LV2 plugin URI, or the VST3 class id as 32 upper-case hex digits
    std::string name;
    std::string version;
    bool operator==(const PluginRef& o) const {
        return format == o.format && id == o.id && name == o.name && version == o.version;
    }
};

// By stable key, never by port index or dense index: both change between
// plugin versions and scans.
struct ParamValue {
    std::string key;      // LV2 port symbol, or the VST3 ParamID in decimal
    float value = 0.0f;   // format-native (ADR-0012 §5.2): LV2 plain, VST3 normalised
    bool operator==(const ParamValue& o) const { return key == o.key && value == o.value; }
};

// The plugin's own state lives in a file beside the session, never inline:
// some run to megabytes, and the document crosses the control socket whole
// with every Snapshot.
struct StateRef {
    std::string file;     // plugin_state_file(sha256)
    std::string sha256;   // 64 lower-case hex digits
    std::uint64_t bytes = 0;
    bool operator==(const StateRef& o) const {
        return file == o.file && sha256 == o.sha256 && bytes == o.bytes;
    }
};

// "Plugin State/<sha256>.state": where a state file with this digest lives
// in the session folder.
std::string plugin_state_file(const std::string& sha256);

// Which plugin sits in which slot, and whether it is in the chain.
struct Insert {
    Id id = 0;
    std::uint8_t slot = 0;   // below kInsertSlots; unique on the track, gaps allowed
    PluginRef plugin;
    bool active = true;      // false: not instantiated, not in the chain, no latency
    bool operator==(const Insert& o) const {
        return id == o.id && slot == o.slot && plugin == o.plugin && active == o.active;
    }
};

// The undoable part of a track. Input, monitor mode and the mix live in
// TrackMix, because a fader move is not an edit.
struct TrackEdit {
    Id id = 0;
    std::string name;
    std::uint16_t channels = 1;
    Id active_playlist = 0;
    std::vector<std::shared_ptr<const Playlist>> playlists;
    // Sorted by slot; signal flows in slot order, pre-fader. Here and not in
    // MixState because insert structure is undoable (owner decision O1's
    // default, ADR-0012). If the owner rules otherwise this list moves to
    // MixState and the session file stays as it is.
    std::vector<Insert> inserts;
    // Audio tracks hold `playlists`; MIDI and instrument tracks hold
    // `midi_playlists` instead, and `active_playlist` names one of those. A
    // MIDI track has no audio: `channels` is 0 and it holds no insert.
    TrackKind kind = TrackKind::Audio;
    std::vector<std::shared_ptr<const MidiPlaylist>> midi_playlists;
    bool operator==(const TrackEdit& o) const;
};

struct Marker {
    Id id = 0;
    Frames position = 0;
    std::string name;
    Frames length = 0;   // > 0 makes it a range
    bool operator==(const Marker& o) const {
        return id == o.id && position == o.position && name == o.name &&
               length == o.length;
    }
};

struct MarkerList {
    std::vector<Marker> markers;   // sorted by (position, id)
};

// One undo step's worth of state.
struct EditState {
    std::vector<std::shared_ptr<const TrackEdit>> tracks;   // display order
    std::shared_ptr<const MarkerList> markers;
    bool operator==(const EditState& o) const;
};

struct Source {
    Id id = 0;
    std::string path;           // relative to the session folder, '/'-separated
    std::uint16_t channels = 1;
    Frames length = 0;
    AudioFormat format = AudioFormat::Int24;
    std::int64_t time_reference = 0;
    std::string umid;           // 64 lower-case hex digits, or empty if none
    SourceKind kind = SourceKind::Recorded;
    // Recorded sources only.
    std::int64_t take = 0;
    Id recorded_on_track = 0;
    Frames latency_compensation = 0;
    bool operator==(const Source& o) const;
};

// A MIDI take or an imported SMF beside the session (midi.md §4.2). The
// regions hold their own events, so a source missing or changed at open
// changes nothing that plays: it says where a region came from.
struct MidiSource {
    Id id = 0;
    std::string path;              // "MIDI Files/<name>.mid", relative, '/'-separated
    Frames length = 0;             // frames from the take's start to its end
    std::uint64_t events = 0;      // what the file held, checked at open
    std::int64_t time_reference = 0;   // samples since midnight at its tick 0
    SourceKind kind = SourceKind::Recorded;
    // Recorded sources only.
    std::int64_t take = 0;
    Id recorded_on_track = 0;
    Frames latency_compensation = 0;
    bool operator==(const MidiSource& o) const;
};

// Append-only: undo never removes a source (session-format.md, rule 2).
struct SourcePool {
    std::vector<Source> sources;            // sorted by id
    std::vector<MidiSource> midi_sources;   // sorted by id, in the same id space
    const Source* find(Id id) const;
    const MidiSource* find_midi(Id id) const;
    bool operator==(const SourcePool& o) const {
        return sources == o.sources && midi_sources == o.midi_sources;
    }
};

// Where a MIDI or instrument track's MIDI comes from and goes. Saved and
// not undoable, like `input` (owner question O5). Ports are named, not
// numbered: a MIDI port is a device that comes and goes while the session
// runs, and an index would point at another one after a replug.
inline constexpr const char* kAllMidiInputs = "all";
inline constexpr const char* kNoMidiOutput = "none";
struct MidiRouting {
    std::string in_port = kAllMidiInputs;   // "all", or a port name
    std::uint8_t in_channel = 0;            // 0 all, else 1 to 16
    std::string out_port = kNoMidiOutput;   // MIDI tracks only: "none", or a port name
    std::uint8_t out_channel = 0;           // 0 as recorded, else 1 to 16
    bool operator==(const MidiRouting& o) const {
        return in_port == o.in_port && in_channel == o.in_channel && out_port == o.out_port &&
               out_channel == o.out_channel;
    }
};

struct TrackMix {
    Id track = 0;
    std::vector<std::uint16_t> input;   // one capture channel per track channel
    MonitorMode monitor = MonitorMode::Auto;
    float gain_db = 0.0f;
    float pan = 0.0f;
    bool mute = false;
    bool solo = false;
    // Never implicitly muted by another track's solo, as Pro Tools' solo
    // safe; the track's own mute still silences it.
    bool solo_safe = false;
    // MIDI and instrument tracks; an audio track's is the default.
    MidiRouting midi;
    bool operator==(const TrackMix& o) const;
};

struct MonitorBus {
    std::array<std::uint16_t, 2> outputs{{0, 1}};
    float gain_db = 0.0f;
    float pan_law_db = -3.0f;
    bool operator==(const MonitorBus& o) const {
        return outputs == o.outputs && gain_db == o.gain_db &&
               pan_law_db == o.pan_law_db;
    }
};

// An insert's values: saved and not undoable, like a fader (ADR-0012 §6.1).
struct InsertMix {
    Id insert = 0;
    bool bypass = false;   // still delayed by its latency, so a toggle never shifts timing
    std::vector<ParamValue> params;   // keys unique
    std::optional<StateRef> state;
    bool operator==(const InsertMix& o) const {
        return insert == o.insert && bypass == o.bypass && params == o.params &&
               state == o.state;
    }
};

// Saved, never undone.
struct MixState {
    MonitorBus bus;
    // Keyed by track id. An entry outlives an undone AddTrack, so redo
    // brings the track back with its faders where they were.
    std::vector<TrackMix> tracks;
    // Keyed by insert id, and outliving an undone AddInsert or a
    // RemoveInsert the same way, so undo and redo bring a plugin back with
    // its settings. Unlike a track's, an entry goes once no step in the undo
    // window holds its insert (History::prune_insert_values): an insert
    // carries up to kMaxInsertParams values.
    std::vector<InsertMix> inserts;
    const TrackMix* find(Id track) const;
    const InsertMix* find_insert(Id insert) const;
    bool operator==(const MixState& o) const {
        return bus == o.bus && tracks == o.tracks && inserts == o.inserts;
    }
};

struct Timecode {
    std::int64_t origin = 172'800'000;   // 01:00:00:00 at 48 kHz
    Fps fps = Fps::F25;
    bool operator==(const Timecode& o) const {
        return origin == o.origin && fps == o.fps;
    }
};

struct TempoEntry {
    Frames at = 0;
    float bpm = 120.0f;
    std::array<std::uint32_t, 2> meter{{4, 4}};
    bool operator==(const TempoEntry& o) const {
        return at == o.at && bpm == o.bpm && meter == o.meter;
    }
};

struct Transport {
    std::array<Frames, 2> loop{{0, 0}};    // [0, 0] is unset
    std::array<Frames, 2> punch{{0, 0}};
    Frames pre_roll = 96000;
    Frames post_roll = 48000;
    Frames punch_crossfade = 240;
    // Bars of click before a take started stopped, when count-off is on
    // (ADR-0020 §3): Pro Tools' default, 1 to kMaxCountOffBars.
    std::uint32_t count_off_bars = 2;
    bool operator==(const Transport& o) const {
        return loop == o.loop && punch == o.punch && pre_roll == o.pre_roll &&
               post_roll == o.post_roll && punch_crossfade == o.punch_crossfade &&
               count_off_bars == o.count_off_bars;
    }
};
inline constexpr std::uint32_t kMaxCountOffBars = 99;

// The most take files a session remembers as left (left_takes).
inline constexpr std::size_t kMaxLeftTakes = 4096;

struct SessionSettings {
    std::string name;
    std::uint32_t sample_rate = 48000;
    AudioFormat audio_format = AudioFormat::Int24;
    Timecode timecode;
    std::vector<TempoEntry> tempo{TempoEntry{}};
    Transport transport;
    // Which session this is, written into every take's Description, so a
    // take is told from another session's of the same name (two unsaved
    // "Untitled" ones). Made with the document (new_session_id()) and kept
    // by every save; empty for a session saved before version 7 until its
    // next save gives it one.
    std::string session_id;
    // Take files in "Audio Files/" the operator chose to leave when they
    // were offered for recovery: never offered again. Sorted, no repeats.
    std::vector<std::string> left_takes;
    bool operator==(const SessionSettings& o) const {
        return name == o.name && sample_rate == o.sample_rate &&
               audio_format == o.audio_format && timecode == o.timecode &&
               tempo == o.tempo && transport == o.transport &&
               session_id == o.session_id && left_takes == o.left_takes;
    }
};

// A fresh session id: 128 random bits as a lowercase RFC 4122 version 4
// UUID. Not on the audio thread: it reads the system's entropy source.
std::string new_session_id();
// Empty, or 1 to 64 of [0-9a-f-]: one word of a take's Description.
bool valid_session_id(const std::string& id);

// What a save, a plan build or a replica reads.
struct Snapshot {
    std::shared_ptr<const EditState> edit;
    std::shared_ptr<const SourcePool> sources;
    MixState mix;
    SessionSettings settings;
    // Bumped by every applied operation, undo and redo. A replica checks
    // each message's revision is the next one.
    std::uint64_t revision = 0;
    // Every id an operation introduces must be at least this, which makes
    // uniqueness an O(1) check. Not saved: a load resumes at max(id) + 1.
    Id next_id = 1;
};

// An empty session with these settings: no track, no source, no marker.
Snapshot new_session(const SessionSettings& settings);

// Everything a replica must agree on, revision and id counter included.
bool operator==(const Snapshot& a, const Snapshot& b);
inline bool operator!=(const Snapshot& a, const Snapshot& b) { return !(a == b); }

// What the session file holds: the edit, the pool, the settings and the
// mix of the tracks that exist. Not the revision, the id counter, or a mix
// entry left behind by an undone AddTrack.
bool same_document(const Snapshot& a, const Snapshot& b);

// Every invariant of the session format (session-format.md) over the whole
// snapshot. On failure `error` names the object by its key path, as the
// file would ("tracks[1].playlists[0].regions[3]: ...").
bool check_invariants(const Snapshot& s, std::string& error);

}  // namespace sonora::model
