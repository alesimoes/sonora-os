// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/model/model.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include "checks.h"
#include "sonora/json/json.h"

namespace sonora::model {

namespace {

std::string str(std::int64_t n) { return std::to_string(n); }
std::string str(std::uint64_t n) { return std::to_string(n); }

bool in_range(std::int64_t n) { return n >= 0 && n <= kMaxInteger; }

// Written so that NaN fails it.
bool gain_in_range(float db) { return db >= kMinGainDb && db <= kMaxGainDb; }

std::string gain_range() {
    return "must be from " + str(static_cast<std::int64_t>(kMinGainDb)) + " to " +
           str(static_cast<std::int64_t>(kMaxGainDb)) + " dB";
}

// A crossfade or a region needs this much of every source it names; the
// shortest one decides. -1 when a source is missing.
Frames shortest_source(const Region& r, const SourcePool& pool) {
    Frames shortest = kMaxInteger;
    for (const SourceRef& c : r.channels) {
        const Source* s = pool.find(c.source);
        if (s == nullptr) return -1;
        shortest = std::min(shortest, s->length);
    }
    return shortest;
}

template <typename T>
bool same_ptr_or_value(const std::shared_ptr<const T>& a,
                       const std::shared_ptr<const T>& b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return *a == *b;
}

}  // namespace

void crossfade_extent(const Crossfade& x, Frames& before, Frames& after) {
    switch (x.align) {
        case Align::Pre:
            before = x.length;
            after = 0;
            return;
        case Align::Post:
            before = 0;
            after = x.length;
            return;
        case Align::Centre:
            break;
    }
    before = x.length / 2;
    after = x.length - before;
}

bool Region::operator==(const Region& o) const {
    return id == o.id && name == o.name && position == o.position &&
           length == o.length && source_offset == o.source_offset &&
           channels == o.channels && gain_db == o.gain_db && mute == o.mute &&
           fade_in == o.fade_in && fade_out == o.fade_out;
}

bool Playlist::operator==(const Playlist& o) const {
    return id == o.id && name == o.name && regions == o.regions &&
           crossfades == o.crossfades;
}

bool TrackEdit::operator==(const TrackEdit& o) const {
    if (id != o.id || name != o.name || channels != o.channels ||
        active_playlist != o.active_playlist || inserts != o.inserts ||
        playlists.size() != o.playlists.size()) {
        return false;
    }
    for (std::size_t i = 0; i < playlists.size(); ++i) {
        if (!same_ptr_or_value(playlists[i], o.playlists[i])) return false;
    }
    return true;
}

bool EditState::operator==(const EditState& o) const {
    if (tracks.size() != o.tracks.size()) return false;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (!same_ptr_or_value(tracks[i], o.tracks[i])) return false;
    }
    if (markers == o.markers) return true;
    if (!markers || !o.markers) return false;
    return markers->markers == o.markers->markers;
}

bool Source::operator==(const Source& o) const {
    return id == o.id && path == o.path && channels == o.channels &&
           length == o.length && format == o.format &&
           time_reference == o.time_reference && umid == o.umid &&
           kind == o.kind && take == o.take &&
           recorded_on_track == o.recorded_on_track &&
           latency_compensation == o.latency_compensation;
}

bool TrackMix::operator==(const TrackMix& o) const {
    return track == o.track && input == o.input && monitor == o.monitor &&
           gain_db == o.gain_db && pan == o.pan && mute == o.mute &&
           solo == o.solo;
}

const Source* SourcePool::find(Id id) const {
    const auto it = std::lower_bound(
        sources.begin(), sources.end(), id,
        [](const Source& s, Id want) { return s.id < want; });
    if (it == sources.end() || it->id != id) return nullptr;
    return &*it;
}

const TrackMix* MixState::find(Id track) const {
    for (const TrackMix& m : tracks) {
        if (m.track == track) return &m;
    }
    return nullptr;
}

const InsertMix* MixState::find_insert(Id insert) const {
    for (const InsertMix& m : inserts) {
        if (m.insert == insert) return &m;
    }
    return nullptr;
}

std::string plugin_state_file(const std::string& sha256) {
    return "Plugin State/" + sha256 + ".state";
}

Snapshot new_session(const SessionSettings& settings) {
    auto edit = std::make_shared<EditState>();
    edit->markers = std::make_shared<const MarkerList>();
    Snapshot s;
    s.edit = std::move(edit);
    s.sources = std::make_shared<const SourcePool>();
    s.settings = settings;
    return s;
}

bool operator==(const Snapshot& a, const Snapshot& b) {
    return same_ptr_or_value(a.edit, b.edit) &&
           (a.sources == b.sources ||
            (a.sources && b.sources && a.sources->sources == b.sources->sources)) &&
           a.mix == b.mix && a.settings == b.settings &&
           a.revision == b.revision && a.next_id == b.next_id;
}

bool same_document(const Snapshot& a, const Snapshot& b) {
    if (!same_ptr_or_value(a.edit, b.edit)) return false;
    if (!(a.sources == b.sources ||
          (a.sources && b.sources && a.sources->sources == b.sources->sources))) {
        return false;
    }
    if (!(a.settings == b.settings) || !(a.mix.bus == b.mix.bus)) return false;
    for (const auto& t : a.edit->tracks) {
        const TrackMix* ma = a.mix.find(t->id);
        const TrackMix* mb = b.mix.find(t->id);
        if (ma == nullptr || mb == nullptr || !(*ma == *mb)) return false;
        for (const Insert& i : t->inserts) {
            const InsertMix* ia = a.mix.find_insert(i.id);
            const InsertMix* ib = b.mix.find_insert(i.id);
            if (ia == nullptr || ib == nullptr || !(*ia == *ib)) return false;
        }
    }
    return true;
}

namespace detail {

bool valid_text(const std::string& s) { return json::valid_utf8(s); }

std::string source_problem(const Source& s) {
    if (s.id == 0) return "id must be at least 1";
    if (!valid_text(s.path)) return "path is not valid UTF-8";
    // The folder is the unit: a path that leaves it, or that only means
    // something on this machine, breaks the session when the folder moves.
    if (s.path.empty()) return "path is empty";
    if (s.path.front() == '/') return "path \"" + s.path + "\" is absolute";
    if (s.path.find('\\') != std::string::npos) {
        return "path \"" + s.path + "\" uses '\\'; paths are '/'-separated";
    }
    if (s.path.find(':') != std::string::npos) {
        return "path \"" + s.path + "\" contains ':'";
    }
    std::size_t start = 0;
    for (;;) {
        const std::size_t slash = s.path.find('/', start);
        const std::string part = s.path.substr(
            start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() || part == "." || part == "..") {
            return "path \"" + s.path +
                   "\" has an empty, \".\" or \"..\" component";
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    if (s.channels == 0) return "channels must be at least 1";
    if (!in_range(s.length)) return "length is out of range";
    if (!in_range(s.time_reference)) return "time_reference is out of range";
    if (!s.umid.empty()) {
        bool hex = s.umid.size() == 64;
        for (const char c : s.umid) {
            hex = hex && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        }
        if (!hex) return "umid must be 64 lower-case hex digits";
    }
    if (s.kind == SourceKind::Imported) {
        if (s.take != 0 || s.recorded_on_track != 0 || s.latency_compensation != 0) {
            return "an imported source has no take, recorded_on_track or"
                   " latency_compensation";
        }
    } else {
        if (!in_range(s.take)) return "take is out of range";
        if (s.recorded_on_track > static_cast<Id>(kMaxInteger)) {
            return "recorded_on_track is out of range";
        }
        if (!in_range(s.latency_compensation)) {
            return "latency_compensation is out of range";
        }
    }
    return {};
}

std::string region_problem(const Region& r, const SourcePool& pool,
                           std::uint16_t track_channels) {
    if (r.id == 0) return "id must be at least 1";
    if (!valid_text(r.name)) return "name is not valid UTF-8";
    if (r.length <= 0) return "length must be greater than 0";
    if (r.position < 0) return "position must not be negative";
    if (r.source_offset < 0) return "source_offset must not be negative";
    if (r.position > kMaxInteger || r.length > kMaxInteger ||
        r.position + r.length > kMaxInteger) {
        return "position + length is past the end of the timeline";
    }
    if (r.source_offset > kMaxInteger) return "source_offset is out of range";
    if (r.channels.size() != track_channels) {
        return "has " + str(static_cast<std::int64_t>(r.channels.size())) +
               " source entries for a " +
               str(static_cast<std::int64_t>(track_channels)) + "-channel track";
    }
    for (const SourceRef& c : r.channels) {
        const Source* s = pool.find(c.source);
        if (s == nullptr) return "source " + str(c.source) + " does not exist";
        if (c.channel >= s->channels) {
            return "source " + str(c.source) + " has no channel " +
                   str(static_cast<std::int64_t>(c.channel));
        }
        if (r.source_offset + r.length > s->length) {
            return "source_offset + length (" + str(r.source_offset + r.length) +
                   ") is past the end of source " + str(c.source) + " (length " +
                   str(s->length) + ")";
        }
    }
    if (r.fade_in.length < 0 || r.fade_out.length < 0) {
        return "a fade length must not be negative";
    }
    if (r.fade_in.length > r.length - r.fade_out.length) {
        return "fade_in.length + fade_out.length is longer than the region";
    }
    if (!gain_in_range(r.gain_db)) return "gain_db " + gain_range();
    return {};
}

const Region* find_region(const Playlist& p, Id id) {
    for (const Region& r : p.regions) {
        if (r.id == id) return &r;
    }
    return nullptr;
}

std::string crossfade_problem(const Crossfade& x, const Playlist& p,
                              const SourcePool& pool) {
    return crossfade_problem(x, find_region(p, x.left), find_region(p, x.right), pool);
}

std::string crossfade_problem(const Crossfade& x, const Region* l, const Region* r,
                              const SourcePool& pool) {
    if (x.length <= 0 || x.length > kMaxInteger) {
        return "length must be greater than 0";
    }
    if (l == nullptr) return "region " + str(x.left) + " is not on this playlist";
    if (r == nullptr) return "region " + str(x.right) + " is not on this playlist";
    if (l->end() != r->position) {
        return "regions " + str(x.left) + " and " + str(x.right) +
               " do not butt (" + str(x.left) + " ends at " + str(l->end()) +
               ", " + str(x.right) + " starts at " + str(r->position) + ")";
    }
    Frames before = 0, after = 0;
    crossfade_extent(x, before, after);
    if (before > l->length || after > r->length) {
        return "longer than the regions it joins";
    }
    // Refused, not shortened: a crossfade quietly shorter than asked for is
    // a different edit from the one the operator made.
    const Frames left_handle = shortest_source(*l, pool) - l->source_offset - l->length;
    if (left_handle < after) {
        return "region " + str(x.left) + " has " + str(left_handle) +
               " frames of handle after its end; the crossfade needs " + str(after);
    }
    if (r->source_offset < before) {
        return "region " + str(x.right) + " has " + str(r->source_offset) +
               " frames of handle before its start; the crossfade needs " +
               str(before);
    }
    return {};
}

void sort_crossfades(Playlist& p) {
    // The map below is over every region; a playlist with nothing to sort
    // is most of them, and every member of a compound comes through here.
    if (p.crossfades.size() < 2) return;
    std::unordered_map<Id, Frames> pos;
    for (const Region& r : p.regions) pos[r.id] = r.position;
    std::stable_sort(p.crossfades.begin(), p.crossfades.end(),
                     [&](const Crossfade& a, const Crossfade& b) {
                         return pos[a.left] < pos[b.left];
                     });
}

void sort_markers(MarkerList& m) {
    std::stable_sort(m.markers.begin(), m.markers.end(),
                     [](const Marker& a, const Marker& b) {
                         if (a.position != b.position) return a.position < b.position;
                         return a.id < b.id;
                     });
}

std::vector<FadeFit> fit_fades_to_crossfades(Playlist& p) {
    struct Reach {
        Frames in = -1;    // -1: no crossfade at that end
        Frames out = -1;
    };
    std::unordered_map<Id, Reach> reach;
    for (const Crossfade& x : p.crossfades) {
        Frames before = 0, after = 0;
        crossfade_extent(x, before, after);
        reach[x.left].out = before;
        reach[x.right].in = after;
    }
    std::vector<FadeFit> fits;
    for (std::size_t i = 0; i < p.regions.size(); ++i) {
        Region& r = p.regions[i];
        const auto it = reach.find(r.id);
        if (it == reach.end()) continue;
        const Reach& x = it->second;
        // With crossfades at both ends both fades are unheard; only a fade
        // at an end without one can run into the other end's crossfade. A
        // crossfade longer than the region is left for playlist_problem to
        // refuse, not turned into a negative fade.
        if (x.in >= 0 && x.out < 0 && x.in <= r.length &&
            r.fade_out.length > r.length - x.in) {
            fits.push_back({i, false, r.fade_out.length, r.length - x.in});
            r.fade_out.length = r.length - x.in;
        } else if (x.out >= 0 && x.in < 0 && x.out <= r.length &&
                   r.fade_in.length > r.length - x.out) {
            fits.push_back({i, true, r.fade_in.length, r.length - x.out});
            r.fade_in.length = r.length - x.out;
        }
    }
    return fits;
}

std::string playlist_problem(const Playlist& p, const SourcePool& pool,
                             std::uint16_t track_channels, std::string& where) {
    where.clear();
    if (p.id == 0) return "id must be at least 1";
    if (!valid_text(p.name)) return "name is not valid UTF-8";
    std::unordered_map<Id, std::size_t> index;
    for (std::size_t i = 0; i < p.regions.size(); ++i) {
        const Region& r = p.regions[i];
        std::string why = region_problem(r, pool, track_channels);
        if (why.empty()) why = order_problem(p, i);
        if (why.empty() && !index.emplace(r.id, i).second) {
            why = "region id " + str(r.id) + " appears twice";
        }
        if (!why.empty()) {
            where = "regions[" + str(static_cast<std::int64_t>(i)) + "]";
            return why;
        }
    }
    // At most two regions sound at any instant: the crossfades at the two
    // ends of one region must not meet inside it.
    std::vector<Frames> in_after(p.regions.size(), 0);
    std::vector<Frames> out_before(p.regions.size(), 0);
    std::vector<bool> has_in(p.regions.size(), false);
    std::vector<bool> has_out(p.regions.size(), false);
    for (std::size_t k = 0; k < p.crossfades.size(); ++k) {
        const Crossfade& x = p.crossfades[k];
        where = "crossfades[" + str(static_cast<std::int64_t>(k)) + "]";
        std::string why = crossfade_problem(x, p, pool);
        if (!why.empty()) return why;
        const std::size_t li = index[x.left];
        const std::size_t ri = index[x.right];
        if (has_out[li] || has_in[ri]) return second_crossfade_problem(x);
        if (k > 0 && index[p.crossfades[k - 1].left] > li) return kCrossfadesUnsorted;
        Frames before = 0, after = 0;
        crossfade_extent(x, before, after);
        has_out[li] = true;
        out_before[li] = before;
        has_in[ri] = true;
        in_after[ri] = after;
    }
    // What is heard at an end is its crossfade if it has one, else its own
    // fade. A fade under a crossfade at the same end is kept but silent; one
    // at the other end that runs into a crossfade would be multiplied by it.
    for (std::size_t i = 0; i < p.regions.size(); ++i) {
        std::string why =
            heard_problem(p.regions[i], has_in[i], in_after[i], has_out[i], out_before[i]);
        if (why.empty()) continue;
        where = "regions[" + str(static_cast<std::int64_t>(i)) + "]";
        return why;
    }
    where.clear();
    return {};
}

std::string order_problem(const Playlist& p, std::size_t i) {
    if (i == 0 || p.regions[i].position >= p.regions[i - 1].end()) return {};
    return p.regions[i - 1].position <= p.regions[i].position
               ? "overlaps the region before it (regions[" +
                     str(static_cast<std::int64_t>(i - 1)) + "])"
               : "is not sorted by position";
}

std::string second_crossfade_problem(const Crossfade& x) {
    return "a second crossfade between regions " + str(x.left) + " and " + str(x.right);
}

const char* const kCrossfadesUnsorted = "crossfades are not sorted by position";

std::string heard_problem(const Region& r, bool has_in, Frames in_after, bool has_out,
                          Frames out_before) {
    const Frames heard_in = has_in ? in_after : r.fade_in.length;
    const Frames heard_out = has_out ? out_before : r.fade_out.length;
    if (heard_in + heard_out <= r.length) return {};
    if (has_in && has_out) {
        return "the crossfades at its two ends overlap; at most two"
               " regions may sound at once";
    }
    return has_in ? "its fade-out overlaps the crossfade at its start"
                  : "its fade-in overlaps the crossfade at its end";
}

std::string mix_problem(const TrackMix& m, std::uint16_t track_channels,
                        std::string& where) {
    where.clear();
    if (m.input.size() != track_channels) {
        where = "input";
        return "needs one capture channel per track channel (" +
               str(static_cast<std::int64_t>(track_channels)) + ")";
    }
    if (!gain_in_range(m.gain_db)) {
        where = "gain_db";
        return gain_range();
    }
    if (!(m.pan >= -1.0f && m.pan <= 1.0f)) {
        where = "pan";
        return "must be from -1 to 1";
    }
    return {};
}

std::string bus_problem(const MonitorBus& b, std::string& where) {
    where.clear();
    if (!gain_in_range(b.gain_db)) {
        where = "gain_db";
        return gain_range();
    }
    // Written so that NaN fails it.
    if (!(b.pan_law_db >= kMinPanLawDb && b.pan_law_db <= 0.0f)) {
        where = "pan_law_db";
        return "must be from " + str(static_cast<std::int64_t>(kMinPanLawDb)) + " to 0";
    }
    return {};
}

std::string settings_problem(const SessionSettings& s, std::string& where) {
    where.clear();
    if (!valid_text(s.name)) {
        where = "name";
        return "is not valid UTF-8";
    }
    if (s.sample_rate == 0) {
        where = "sample_rate";
        return "must be greater than 0";
    }
    if (!in_range(s.timecode.origin)) {
        where = "timecode.origin";
        return "is out of range";
    }
    if (s.tempo.empty()) {
        where = "tempo";
        return "needs at least one entry";
    }
    for (std::size_t i = 0; i < s.tempo.size(); ++i) {
        const TempoEntry& t = s.tempo[i];
        where = "tempo[" + str(static_cast<std::int64_t>(i)) + "]";
        if (i == 0 && t.at != 0) return "the first entry must be at 0";
        if (!in_range(t.at)) return "at is out of range";
        if (i > 0 && t.at <= s.tempo[i - 1].at) {
            return "entries must be sorted by at, without duplicates";
        }
        if (!(std::isfinite(t.bpm) && t.bpm > 0.0f)) {
            return "bpm must be a positive number";
        }
        if (t.meter[0] == 0 || t.meter[1] == 0) return "meter must be two counts of at least 1";
    }
    const Transport& tr = s.transport;
    for (const auto& range : {std::make_pair("transport.loop", tr.loop),
                              std::make_pair("transport.punch", tr.punch)}) {
        if (!in_range(range.second[0]) || !in_range(range.second[1]) ||
            range.second[0] > range.second[1]) {
            where = range.first;
            return "must be [start, end] with start <= end";
        }
    }
    if (!in_range(tr.pre_roll)) {
        where = "transport.pre_roll";
        return "is out of range";
    }
    if (!in_range(tr.post_roll)) {
        where = "transport.post_roll";
        return "is out of range";
    }
    if (!in_range(tr.punch_crossfade)) {
        where = "transport.punch_crossfade";
        return "is out of range";
    }
    where.clear();
    return {};
}

std::string marker_problem(const Marker& m) {
    if (m.id == 0) return "id must be at least 1";
    if (!valid_text(m.name)) return "name is not valid UTF-8";
    if (!in_range(m.position) || !in_range(m.length) ||
        m.position + m.length > kMaxInteger) {
        return "position + length is out of range";
    }
    return {};
}

static bool hex_digits(const std::string& s, std::size_t n, char ten) {
    if (s.size() != n) return false;
    for (const char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= ten && c <= ten + 5))) return false;
    }
    return true;
}

// The LV2 spec's grammar for a port symbol: a C identifier.
static bool lv2_symbol(const std::string& s) {
    if (s.empty()) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        if (!(alpha || (i > 0 && c >= '0' && c <= '9'))) return false;
    }
    return true;
}

// A VST3 ParamID is a uint32, written in decimal without leading zeros, so
// one parameter has one spelling.
static bool vst3_param_id(const std::string& s) {
    if (s.empty() || s.size() > 10 || (s.size() > 1 && s[0] == '0')) return false;
    std::uint64_t n = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
        n = n * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return n <= 0xFFFFFFFFu;
}

std::string plugin_problem(const PluginRef& p, std::string& where) {
    where = "plugin.id";
    if (p.id.empty()) return "is empty";
    if (p.format == PluginFormat::Lv2) {
        if (!valid_text(p.id)) return "is not valid UTF-8";
        for (const char c : p.id) {
            if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F) {
                return "an LV2 plugin URI has no space or control character";
            }
        }
        if (p.id.find(':') == std::string::npos) {
            return "an LV2 plugin URI needs a scheme (\"http:\", \"urn:\")";
        }
    } else if (!hex_digits(p.id, 32, 'A')) {
        return "a VST3 class id is 32 upper-case hex digits";
    }
    where = "plugin.name";
    if (!valid_text(p.name)) return "is not valid UTF-8";
    where = "plugin.version";
    if (!valid_text(p.version)) return "is not valid UTF-8";
    where.clear();
    return {};
}

std::string insert_param_count_problem(std::size_t count) {
    if (count <= kMaxInsertParams) return {};
    return "holds " + str(static_cast<std::int64_t>(count)) + " values; an insert holds at most " +
           str(static_cast<std::int64_t>(kMaxInsertParams));
}

std::string insert_mix_problem(const InsertMix& m, PluginFormat format, std::string& where) {
    where = "params";
    std::string why = insert_param_count_problem(m.params.size());
    if (!why.empty()) return why;
    std::unordered_set<std::string> keys;
    for (std::size_t i = 0; i < m.params.size(); ++i) {
        const ParamValue& p = m.params[i];
        where = "params[" + str(static_cast<std::int64_t>(i)) + "]";
        if (format == PluginFormat::Lv2 && !lv2_symbol(p.key)) {
            return "key \"" + (valid_text(p.key) ? p.key : std::string("?")) +
                   "\" is not an LV2 port symbol";
        }
        if (format == PluginFormat::Vst3 && !vst3_param_id(p.key)) {
            return "key \"" + (valid_text(p.key) ? p.key : std::string("?")) +
                   "\" is not a VST3 ParamID in decimal";
        }
        if (!keys.insert(p.key).second) return "key \"" + p.key + "\" appears twice";
        if (!std::isfinite(p.value)) return "value must be a finite number";
        // Written so that NaN fails it, though isfinite already has.
        if (format == PluginFormat::Vst3 && !(p.value >= 0.0f && p.value <= 1.0f)) {
            return "a VST3 value is normalised, from 0 to 1";
        }
    }
    if (m.state) {
        const StateRef& st = *m.state;
        where = "state";
        if (!hex_digits(st.sha256, 64, 'a')) return "sha256 must be 64 lower-case hex digits";
        if (st.file != plugin_state_file(st.sha256)) {
            return "file must be \"" + plugin_state_file(st.sha256) + "\"";
        }
        if (st.bytes > static_cast<std::uint64_t>(kMaxInteger)) return "bytes is out of range";
    }
    where.clear();
    return {};
}

std::string inserts_problem(const std::vector<Insert>& inserts, std::string& where) {
    for (std::size_t i = 0; i < inserts.size(); ++i) {
        const Insert& in = inserts[i];
        where = "inserts[" + str(static_cast<std::int64_t>(i)) + "]";
        if (in.id == 0) return "id must be at least 1";
        if (in.slot >= kInsertSlots) {
            return "slot " + str(static_cast<std::int64_t>(in.slot)) + " is not one of 0 to " +
                   str(static_cast<std::int64_t>(kInsertSlots - 1));
        }
        if (i > 0 && in.slot == inserts[i - 1].slot) {
            return "slot " + str(static_cast<std::int64_t>(in.slot)) + " is used twice";
        }
        if (i > 0 && in.slot < inserts[i - 1].slot) return "inserts are not sorted by slot";
        std::string inner;
        const std::string why = plugin_problem(in.plugin, inner);
        if (!why.empty()) {
            where += "." + inner;
            return why;
        }
    }
    where.clear();
    return {};
}

}  // namespace detail

bool check_invariants(const Snapshot& s, std::string& error) {
    using namespace detail;
    if (!s.edit || !s.sources || !s.edit->markers) {
        error = "the snapshot is missing its edit, sources or markers";
        return false;
    }
    std::string where;
    std::string why = settings_problem(s.settings, where);
    if (!why.empty()) {
        error = where + ": " + why;
        return false;
    }
    why = bus_problem(s.mix.bus, where);
    if (!why.empty()) {
        error = "monitor_bus." + where + ": " + why;
        return false;
    }

    std::unordered_set<Id> ids;
    const auto claim = [&](Id id, const std::string& path) {
        if (id > static_cast<Id>(kMaxInteger)) {
            error = path + ": id " + std::to_string(id) + " is out of range";
            return false;
        }
        if (id >= s.next_id) {
            error = path + ": id " + std::to_string(id) +
                    " is not below the id counter (" + std::to_string(s.next_id) + ")";
            return false;
        }
        if (!ids.insert(id).second) {
            error = path + ": id " + std::to_string(id) + " is used twice";
            return false;
        }
        return true;
    };

    const auto& sources = s.sources->sources;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const std::string path = "sources[" + std::to_string(i) + "]";
        why = source_problem(sources[i]);
        if (why.empty() && i > 0 && sources[i].id <= sources[i - 1].id) {
            why = "sources are not sorted by id";
        }
        if (!why.empty()) {
            error = path + ": " + why;
            return false;
        }
        if (!claim(sources[i].id, path)) return false;
    }

    const auto& tracks = s.edit->tracks;
    for (std::size_t t = 0; t < tracks.size(); ++t) {
        const TrackEdit& tr = *tracks[t];
        const std::string path = "tracks[" + std::to_string(t) + "]";
        if (tr.id == 0 || !valid_text(tr.name) || tr.channels < 1 || tr.channels > 2) {
            error = path + ": needs an id, a UTF-8 name and 1 or 2 channels";
            return false;
        }
        if (!claim(tr.id, path)) return false;
        if (tr.playlists.empty()) {
            error = path + ": a track needs at least one playlist";
            return false;
        }
        const TrackMix* mix = s.mix.find(tr.id);
        if (mix == nullptr) {
            error = path + ": has no mix";
            return false;
        }
        why = mix_problem(*mix, tr.channels, where);
        if (!why.empty()) {
            error = path + "." + where + ": " + why;
            return false;
        }
        why = inserts_problem(tr.inserts, where);
        if (!why.empty()) {
            error = path + "." + where + ": " + why;
            return false;
        }
        for (std::size_t i = 0; i < tr.inserts.size(); ++i) {
            const Insert& in = tr.inserts[i];
            const std::string ipath = path + ".inserts[" + std::to_string(i) + "]";
            if (!claim(in.id, ipath)) return false;
            const InsertMix* im = s.mix.find_insert(in.id);
            if (im == nullptr) {
                error = ipath + ": has no values (bypass, params, state)";
                return false;
            }
            why = insert_mix_problem(*im, in.plugin.format, where);
            if (!why.empty()) {
                error = ipath + "." + where + ": " + why;
                return false;
            }
        }
        bool active_found = false;
        for (std::size_t p = 0; p < tr.playlists.size(); ++p) {
            const Playlist& pl = *tr.playlists[p];
            const std::string ppath = path + ".playlists[" + std::to_string(p) + "]";
            active_found = active_found || pl.id == tr.active_playlist;
            why = playlist_problem(pl, *s.sources, tr.channels, where);
            if (!why.empty()) {
                error = ppath + (where.empty() ? "" : "." + where) + ": " + why;
                return false;
            }
            if (!claim(pl.id, ppath)) return false;
            for (std::size_t r = 0; r < pl.regions.size(); ++r) {
                if (!claim(pl.regions[r].id,
                           ppath + ".regions[" + std::to_string(r) + "]")) {
                    return false;
                }
            }
        }
        if (!active_found) {
            error = path + ".active_playlist: names no playlist of this track";
            return false;
        }
    }

    const auto& markers = s.edit->markers->markers;
    for (std::size_t i = 0; i < markers.size(); ++i) {
        const std::string path = "markers[" + std::to_string(i) + "]";
        why = marker_problem(markers[i]);
        if (why.empty() && i > 0 &&
            (markers[i].position < markers[i - 1].position ||
             (markers[i].position == markers[i - 1].position &&
              markers[i].id < markers[i - 1].id))) {
            why = "markers are not sorted by position";
        }
        if (!why.empty()) {
            error = path + ": " + why;
            return false;
        }
        if (!claim(markers[i].id, path)) return false;
    }
    return true;
}

}  // namespace sonora::model
