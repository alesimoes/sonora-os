// SPDX-License-Identifier: GPL-3.0-or-later
//
// The assistant contract (ADR-0155): signatures that keep audio and the key
// out of a request, the opt-in default, and the fake transport the core and
// UI slices test against. Behaviour of build_request and parse_reply is
// tested with their implementations (assistant/src/core).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <type_traits>

#include "sonora/assistant/fake_transport.h"
#include "sonora/assistant/feature_bundle.h"
#include "sonora/assistant/key_store.h"
#include "sonora/assistant/reply_parser.h"
#include "sonora/assistant/request.h"
#include "sonora/assistant/settings.h"

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);      \
            std::exit(1);                                                \
        }                                                                \
    } while (0)

using namespace sonora::assistant;

// build_request sees the bundle and a byte cap, nothing else: no sample
// buffer, no snapshot, no key. A new parameter is a privacy change and must
// fail here first.
static_assert(std::is_same<decltype(&build_request),
                           bool (*)(const FeatureBundle&, std::size_t, Request&,
                                    std::string&)>::value,
              "build_request takes only the feature bundle and a byte cap");
static_assert(std::is_same<decltype(&parse_reply),
                           bool (*)(const std::string&, const FeatureBundle&,
                                    const sonora::model::Snapshot&, sonora::model::Plan&,
                                    std::string&)>::value,
              "parse_reply produces a model::Plan");
static_assert(std::is_same<decltype(Measurements::band_db),
                           std::array<double, kBands>>::value,
              "band levels are a fixed array of measurements, not a buffer");
static_assert(std::is_abstract<KeyStore>::value && std::is_abstract<HttpTransport>::value,
              "KeyStore and HttpTransport are seams");

namespace {

// The order the core follows (ADR-0155 §1): the switch and the key are read
// before any request is built, so "off" never reaches a transport.
Unavailable ask(const AssistantSettings& s, const KeyStore& keys, HttpTransport& net,
                const Request& req, Response& out) {
    if (!s.enabled) return Unavailable::SwitchedOff;
    std::string key;
    if (!keys.load(key)) return Unavailable::NoKey;
    switch (net.post(req, key, out)) {
    case TransportStatus::Ok: break;
    case TransportStatus::Offline: return Unavailable::Offline;
    default: return Unavailable::ApiError;
    }
    if (out.http_status == 401 || out.http_status == 403) return Unavailable::BadKey;
    return out.http_status == 200 ? Unavailable::None : Unavailable::ApiError;
}

void opt_in_is_off_by_default() {
    AssistantSettings s;
    CHECK(!s.enabled);
    CHECK(s.max_request_bytes == kDefaultMaxRequestBytes);
}

void switched_off_never_calls_the_transport() {
    AssistantSettings s;
    MemoryKeyStore keys;
    std::string err;
    CHECK(keys.save("sk-test-0123456789", err));
    FakeTransport net;
    net.reply(200, "{}");
    Response r;
    CHECK(ask(s, keys, net, Request{"{}"}, r) == Unavailable::SwitchedOff);
    CHECK(net.calls() == 0);
}

void key_travels_beside_the_body() {
    AssistantSettings s;
    s.enabled = true;
    MemoryKeyStore keys;
    std::string err;
    FakeTransport net;
    net.reply(200, "{\"steps\":[]}");
    Response r;
    CHECK(ask(s, keys, net, Request{}, r) == Unavailable::NoKey);
    CHECK(net.calls() == 0);
    CHECK(keys.save("sk-test-0123456789", err));
    CHECK(ask(s, keys, net, Request{"{\"intent\":\"vocal upfront\"}"}, r) == Unavailable::None);
    CHECK(net.calls() == 1);
    CHECK(net.last_key() == "sk-test-0123456789");
    CHECK(net.last_body().find("sk-test") == std::string::npos);
    CHECK(r.body == "{\"steps\":[]}");
}

void failures_name_their_reason() {
    AssistantSettings s;
    s.enabled = true;
    MemoryKeyStore keys;
    std::string err;
    CHECK(keys.save("sk-test-0123456789", err));
    FakeTransport net;   // offline until told otherwise
    Response r;
    CHECK(ask(s, keys, net, Request{}, r) == Unavailable::Offline);
    CHECK(r.body.empty());
    net.fail(TransportStatus::Timeout);
    CHECK(ask(s, keys, net, Request{}, r) == Unavailable::ApiError);
    net.reply(401, "{}");
    CHECK(ask(s, keys, net, Request{}, r) == Unavailable::BadKey);
    net.reply(500, "{}");
    CHECK(ask(s, keys, net, Request{}, r) == Unavailable::ApiError);
    CHECK(net.calls() == 4);

    std::set<std::string> texts;
    for (Unavailable u : {Unavailable::None, Unavailable::SwitchedOff, Unavailable::NoKey,
                          Unavailable::Offline, Unavailable::BadKey, Unavailable::ApiError,
                          Unavailable::TooLarge, Unavailable::BadReply}) {
        const char* t = unavailable_text(u);
        CHECK(t != nullptr && std::strlen(t) > 0);
        texts.insert(t);
    }
    CHECK(texts.size() == 8);   // each reason reads differently
}

void key_store_refuses_empty_and_masks() {
    MemoryKeyStore keys;
    std::string err, key;
    CHECK(!keys.has_key() && !keys.load(key) && key.empty());
    CHECK(!keys.save("", err) && !err.empty());
    CHECK(keys.save("sk-ant-abcdef7f3a", err) && keys.has_key());
    CHECK(mask_key("sk-ant-abcdef7f3a") == "sk-...7f3a");
    CHECK(mask_key("short") == "****");
    CHECK(mask_key("").empty());
    keys.clear();
    CHECK(!keys.has_key());
}

void reply_ops_are_the_revertible_ones() {
    std::set<std::string> ops(kReplyOps.begin(), kReplyOps.end());
    CHECK(ops.size() == kReplyOps.size());
    CHECK(ops.count("set_track_gain") && ops.count("set_bus_gain"));
}

}  // namespace

int main() {
    opt_in_is_off_by_default();
    switched_off_never_calls_the_transport();
    key_travels_beside_the_body();
    failures_name_their_reason();
    key_store_refuses_empty_and_masks();
    reply_ops_are_the_revertible_ones();
    std::puts("ok");
    return 0;
}
