// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/assistant/request.h"

#include <cmath>

#include "sonora/json/json.h"

namespace sonora::assistant {
namespace {

using json::Value;

Value num(double d) {
    return Value::number_float(static_cast<float>(d));
}

Value measurements(const Measurements& m, bool with_bands) {
    Value o = Value::object();
    o.set("measured", Value::boolean(m.measured));
    o.set("integrated_lufs", num(m.integrated_lufs));
    o.set("true_peak_dbtp", num(m.true_peak_dbtp));
    o.set("loudness_range_lu", num(m.loudness_range_lu));
    o.set("crest_db", num(m.crest_db));
    o.set("correlation", num(m.correlation));
    if (with_bands) {
        Value b = Value::array();
        for (double v : m.band_db) b.push(num(v));
        o.set("band_db", std::move(b));
    }
    return o;
}

Value names(const std::vector<std::string>& v) {
    Value a = Value::array();
    for (const auto& s : v) a.push(Value::string(s));
    return a;
}

Value track(const TrackFeatures& t, bool bands, bool inserts) {
    Value o = Value::object();
    o.set("id", Value::integer(static_cast<std::int64_t>(t.track_id)));
    o.set("name", Value::string(t.name));
    o.set("channels", Value::integer(t.channels));
    o.set("gain_db", num(t.gain_db));
    o.set("pan", num(t.pan));
    o.set("muted", Value::boolean(t.muted));
    if (inserts) o.set("inserts", names(t.inserts));
    Value sends = Value::array();
    for (const auto& s : t.sends) {
        Value so = Value::object();
        so.set("id", Value::integer(static_cast<std::int64_t>(s.send_id)));
        so.set("bus", Value::integer(static_cast<std::int64_t>(s.bus_id)));
        so.set("level_db", num(s.level_db));
        so.set("pre_fader", Value::boolean(s.pre_fader));
        sends.push(std::move(so));
    }
    o.set("sends", std::move(sends));
    o.set("measurements", measurements(t.measured, bands));
    return o;
}

Value bus(const BusFeatures& b, bool inserts) {
    Value o = Value::object();
    o.set("id", Value::integer(static_cast<std::int64_t>(b.bus_id)));
    o.set("name", Value::string(b.name));
    o.set("gain_db", num(b.gain_db));
    if (inserts) o.set("inserts", names(b.inserts));
    return o;
}

bool render(const FeatureBundle& f, bool bands, bool inserts, std::string& body,
            std::string& error) {
    Value root = Value::object();
    root.set("base_revision", Value::integer(static_cast<std::int64_t>(f.base_revision)));
    root.set("sample_rate", num(f.sample_rate));
    root.set("intent", Value::string(f.intent));
    root.set("mix", measurements(f.mix, bands));
    Value tracks = Value::array();
    for (const auto& t : f.tracks) tracks.push(track(t, bands, inserts));
    root.set("tracks", std::move(tracks));
    Value buses = Value::array();
    for (const auto& b : f.buses) buses.push(bus(b, inserts));
    root.set("buses", std::move(buses));
    return json::write(root, body, error);
}

}  // namespace

bool build_request(const FeatureBundle& bundle, std::size_t max_bytes, Request& out,
                   std::string& error) {
    out.body.clear();
    // The writer refuses what JSON cannot say; so does the request, rather
    // than sending a session description with a hole in it.
    for (const auto& t : bundle.tracks) {
        if (!json::valid_utf8(t.name)) {
            error = "track name is not valid UTF-8";
            return false;
        }
    }
    if (!json::valid_utf8(bundle.intent)) {
        error = "intent is not valid UTF-8";
        return false;
    }
    std::string body;
    if (!render(bundle, true, true, body, error)) return false;
    std::string dropped;
    if (body.size() > max_bytes) {
        dropped = "per-band spectra dropped";
        if (!render(bundle, false, true, body, error)) return false;
    }
    if (body.size() > max_bytes) {
        dropped = "per-band spectra and insert names dropped";
        if (!render(bundle, false, false, body, error)) return false;
    }
    if (body.size() > max_bytes) {
        error = "request of " + std::to_string(body.size()) + " bytes exceeds " +
                std::to_string(max_bytes) + " even with " + dropped;
        return false;
    }
    out.body = std::move(body);
    return true;
}

}  // namespace sonora::assistant
