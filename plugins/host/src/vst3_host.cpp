// SPDX-License-Identifier: GPL-3.0-or-later
#include "vst3_host.h"

#include "loader.h"

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

namespace sonora::plugin_host {

namespace fs = std::filesystem;
namespace V = Steinberg::Vst;
using Steinberg::FUnknown;
using Steinberg::int32;
using Steinberg::int64;
using Steinberg::tresult;
using Steinberg::TUID;
using Steinberg::uint32;
using plugin::Refusal;

namespace {

constexpr const char* kPrefix = "vst3:";

// The trap behind every channel pointer array (see Vst3Processor): the same
// signalling-NaN pattern as the chain's canaries.
constexpr uint32_t kTrapCanary = 0x7FA5A5A5u;
// Spare channel pointers past each bus's declared count. A plugin that
// declares 2 channels and writes 8 writes 6 of them into the trap.
constexpr int32 kSpareChannels = 8;
constexpr uint32_t kTrapCheckWords = 64;

bool iid_is(const TUID a, const TUID b) { return std::memcmp(a, b, sizeof(TUID)) == 0; }

std::string hex_of(const TUID id) {
    static const char* const digits = "0123456789ABCDEF";
    std::string s;
    for (int i = 0; i < 16; ++i) {
        const auto b = static_cast<unsigned char>(id[i]);
        s += digits[b >> 4];
        s += digits[b & 15];
    }
    return s;
}

bool parse_hex(const std::string& s, TUID out) {
    if (s.size() != 32) return false;
    for (int i = 0; i < 16; ++i) {
        unsigned v = 0;
        for (int k = 0; k < 2; ++k) {
            const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(s[2 * i + k])));
            v <<= 4;
            if (c >= '0' && c <= '9') v |= unsigned(c - '0');
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
            else return false;
        }
        out[i] = static_cast<char>(v);
    }
    return true;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string utf8(const Steinberg::char16* s, std::size_t max) {
    std::string out;
    for (std::size_t i = 0; i < max && s[i]; ++i) {
        uint32_t c = static_cast<uint16_t>(s[i]);
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < max && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<uint16_t>(s[i + 1]) - 0xDC00);
            ++i;
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::string cstr(const char* s, std::size_t max) { return std::string(s, strnlen(s, max)); }

V::SpeakerArrangement arrangement_for(uint32_t channels) {
    if (channels == 1) return V::SpeakerArr::kMono;
    if (channels == 2) return V::SpeakerArr::kStereo;
    return 0;  // no standard arrangement this host asks for
}

uint32_t channels_of(V::SpeakerArrangement a) {
    return static_cast<uint32_t>(V::SpeakerArr::getChannelCount(a));
}

// Host objects outlive every plugin object that holds them (they belong to
// the world or the processor), so their reference counts are not counted.
#define SONORA_UNCOUNTED                                                     \
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }            \
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

// Messages between a plugin's processor and its controller. Plugins create
// them through IHostApplication::createInstance; LSP's processor reports
// latency changes this way, from its own sync thread, and its controller
// turns them into restartComponent(kLatencyChanged). Never created by this
// host on its processing thread.
class Refcounted {
protected:
    std::atomic<uint32> refs_{1};
};

class HostAttributeList final : public V::IAttributeList, Refcounted {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (iid_is(iid, Steinberg::FUnknown_iid) || iid_is(iid, V::IAttributeList_iid)) {
            addRef();
            *obj = this;
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refs_; }
    uint32 PLUGIN_API release() override {
        const uint32 r = --refs_;
        if (r == 0) delete this;
        return r;
    }
    tresult PLUGIN_API setInt(AttrID id, int64 v) override {
        if (!id) return Steinberg::kInvalidArgument;
        ints_[id] = v;
        return Steinberg::kResultOk;
    }
    tresult PLUGIN_API getInt(AttrID id, int64& v) override {
        auto it = id ? ints_.find(id) : ints_.end();
        if (it == ints_.end()) return Steinberg::kResultFalse;
        v = it->second;
        return Steinberg::kResultOk;
    }
    tresult PLUGIN_API setFloat(AttrID id, double v) override {
        if (!id) return Steinberg::kInvalidArgument;
        floats_[id] = v;
        return Steinberg::kResultOk;
    }
    tresult PLUGIN_API getFloat(AttrID id, double& v) override {
        auto it = id ? floats_.find(id) : floats_.end();
        if (it == floats_.end()) return Steinberg::kResultFalse;
        v = it->second;
        return Steinberg::kResultOk;
    }
    tresult PLUGIN_API setString(AttrID id, const V::TChar* s) override {
        if (!id || !s) return Steinberg::kInvalidArgument;
        std::u16string& v = strings_[id];
        v.clear();
        for (; *s; ++s) v += static_cast<char16_t>(*s);
        return Steinberg::kResultOk;
    }
    tresult PLUGIN_API getString(AttrID id, V::TChar* s, uint32 size_bytes) override {
        auto it = id ? strings_.find(id) : strings_.end();
        if (it == strings_.end() || !s || size_bytes < sizeof(V::TChar)) return Steinberg::kResultFalse;
        const std::size_t room = size_bytes / sizeof(V::TChar) - 1;
        const std::size_t n = std::min(room, it->second.size());
        for (std::size_t i = 0; i < n; ++i) s[i] = static_cast<V::TChar>(it->second[i]);
        s[n] = 0;
        return Steinberg::kResultOk;
    }
    tresult PLUGIN_API setBinary(AttrID id, const void* data, uint32 size) override {
        if (!id || (!data && size)) return Steinberg::kInvalidArgument;
        const auto* p = static_cast<const char*>(data);
        binaries_[id].assign(p, p + size);
        return Steinberg::kResultOk;
    }
    tresult PLUGIN_API getBinary(AttrID id, const void*& data, uint32& size) override {
        auto it = id ? binaries_.find(id) : binaries_.end();
        if (it == binaries_.end()) return Steinberg::kResultFalse;
        data = it->second.data();
        size = static_cast<uint32>(it->second.size());
        return Steinberg::kResultOk;
    }

private:
    using AttrID = V::IAttributeList::AttrID;
    ~HostAttributeList() = default;
    std::map<std::string, int64> ints_;
    std::map<std::string, double> floats_;
    std::map<std::string, std::u16string> strings_;
    std::map<std::string, std::vector<char>> binaries_;
};

class HostMessage final : public V::IMessage, Refcounted {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (iid_is(iid, Steinberg::FUnknown_iid) || iid_is(iid, V::IMessage_iid)) {
            addRef();
            *obj = this;
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refs_; }
    uint32 PLUGIN_API release() override {
        const uint32 r = --refs_;
        if (r == 0) delete this;
        return r;
    }
    Steinberg::FIDString PLUGIN_API getMessageID() override { return id_.c_str(); }
    void PLUGIN_API setMessageID(Steinberg::FIDString id) override { id_ = id ? id : ""; }
    V::IAttributeList* PLUGIN_API getAttributes() override { return attributes_; }

private:
    ~HostMessage() { attributes_->release(); }
    std::string id_;
    HostAttributeList* attributes_ = new HostAttributeList;
};

class HostApplication final : public V::IHostApplication {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (iid_is(iid, Steinberg::FUnknown_iid) || iid_is(iid, V::IHostApplication_iid)) {
            *obj = this;
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    SONORA_UNCOUNTED
    tresult PLUGIN_API getName(V::String128 name) override {
        const char* n = "Sonora";
        int i = 0;
        for (; n[i]; ++i) name[i] = static_cast<Steinberg::char16>(n[i]);
        name[i] = 0;
        return Steinberg::kResultOk;
    }
    tresult PLUGIN_API createInstance(TUID cid, TUID iid, void** obj) override {
        *obj = nullptr;
        FUnknown* made = nullptr;
        if (iid_is(cid, V::IMessage_iid)) made = new HostMessage;
        else if (iid_is(cid, V::IAttributeList_iid)) made = new HostAttributeList;
        else return Steinberg::kResultFalse;
        const tresult r = made->queryInterface(iid, obj);
        made->release();
        return r;
    }
};

// Edits come from an editor this host does not open; what matters is the
// latency notice, which can arrive on any thread.
class ComponentHandler final : public V::IComponentHandler {
public:
    explicit ComponentHandler(std::atomic<bool>& latency_dirty) : latency_dirty_(latency_dirty) {}
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (iid_is(iid, Steinberg::FUnknown_iid) || iid_is(iid, V::IComponentHandler_iid)) {
            *obj = this;
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    SONORA_UNCOUNTED
    tresult PLUGIN_API beginEdit(V::ParamID) override { return Steinberg::kResultOk; }
    tresult PLUGIN_API performEdit(V::ParamID, V::ParamValue) override { return Steinberg::kResultOk; }
    tresult PLUGIN_API endEdit(V::ParamID) override { return Steinberg::kResultOk; }
    // Only flags and wakes: the re-read happens on the host's main thread
    // (Processor::control), never here and never on the processing thread.
    tresult PLUGIN_API restartComponent(int32 flags) override {
        if (flags & V::kLatencyChanged) {
            latency_dirty_.store(true, std::memory_order_release);
            notify_control();
        }
        return Steinberg::kResultOk;
    }

private:
    std::atomic<bool>& latency_dirty_;
};

// Component state handed to the controller at load. Not RT.
class MemoryStream final : public Steinberg::IBStream {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (iid_is(iid, Steinberg::FUnknown_iid) || iid_is(iid, Steinberg::IBStream_iid)) {
            *obj = this;
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    SONORA_UNCOUNTED
    tresult PLUGIN_API read(void* buffer, int32 n, int32* got) override {
        const int64 left = static_cast<int64>(data_.size()) - pos_;
        const int32 k = static_cast<int32>(std::max<int64>(0, std::min<int64>(n, left)));
        if (k > 0) std::memcpy(buffer, data_.data() + pos_, static_cast<std::size_t>(k));
        pos_ += k;
        if (got) *got = k;
        return Steinberg::kResultOk;
    }
    tresult PLUGIN_API write(void* buffer, int32 n, int32* put) override {
        if (n < 0) return Steinberg::kInvalidArgument;
        if (pos_ + n > static_cast<int64>(data_.size())) data_.resize(static_cast<std::size_t>(pos_ + n));
        std::memcpy(data_.data() + pos_, buffer, static_cast<std::size_t>(n));
        pos_ += n;
        if (put) *put = n;
        return Steinberg::kResultOk;
    }
    tresult PLUGIN_API seek(int64 pos, int32 mode, int64* result) override {
        int64 base = 0;
        if (mode == kIBSeekCur) base = pos_;
        else if (mode == kIBSeekEnd) base = static_cast<int64>(data_.size());
        const int64 to = base + pos;
        if (to < 0) return Steinberg::kInvalidArgument;
        pos_ = to;
        if (result) *result = pos_;
        return Steinberg::kResultOk;
    }
    tresult PLUGIN_API tell(int64* pos) override {
        if (pos) *pos = pos_;
        return Steinberg::kResultOk;
    }

private:
    std::vector<char> data_;
    int64 pos_ = 0;
};

// Parameter changes for one block. Fixed capacity, filled between blocks by
// set() from the ring and read by the plugin inside process(): nothing here
// allocates. An output-side instance takes what a plugin reports (meters)
// and discards it at the end of the block.
class ParamQueue final : public V::IParamValueQueue {
public:
    static constexpr int32 kMaxPoints = 8;
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (iid_is(iid, Steinberg::FUnknown_iid) || iid_is(iid, V::IParamValueQueue_iid)) {
            *obj = this;
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    SONORA_UNCOUNTED
    V::ParamID PLUGIN_API getParameterId() override { return id_; }
    int32 PLUGIN_API getPointCount() override { return count_; }
    tresult PLUGIN_API getPoint(int32 index, int32& offset, V::ParamValue& value) override {
        if (index < 0 || index >= count_) return Steinberg::kResultFalse;
        offset = points_[index].offset;
        value = points_[index].value;
        return Steinberg::kResultOk;
    }
    tresult PLUGIN_API addPoint(int32 offset, V::ParamValue value, int32& index) override {
        if (count_ >= kMaxPoints) return Steinberg::kResultFalse;
        points_[count_] = {offset, value};
        index = count_++;
        return Steinberg::kResultOk;
    }
    void reset(V::ParamID id) noexcept {
        id_ = id;
        count_ = 0;
    }
    // The ring's semantics: the last value pushed before a block wins, and
    // it applies from the block's first sample.
    void set_first(V::ParamValue value) noexcept {
        points_[0] = {0, value};
        count_ = 1;
    }

private:
    struct Point {
        int32 offset;
        V::ParamValue value;
    };
    V::ParamID id_ = 0;
    int32 count_ = 0;
    Point points_[kMaxPoints]{};
};

class ParamChanges final : public V::IParameterChanges {
public:
    // As many distinct ParamIDs as the ring can deliver in one block (the
    // host drains at most that many per block), so set() from the ring
    // never runs out of room.
    static constexpr int32 kMaxQueues = static_cast<int32>(plugin::kParamRingCapacity);
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (iid_is(iid, Steinberg::FUnknown_iid) || iid_is(iid, V::IParameterChanges_iid)) {
            *obj = this;
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    SONORA_UNCOUNTED
    int32 PLUGIN_API getParameterCount() override { return used_; }
    V::IParamValueQueue* PLUGIN_API getParameterData(int32 index) override {
        return index >= 0 && index < used_ ? &queues_[index] : nullptr;
    }
    V::IParamValueQueue* PLUGIN_API addParameterData(const V::ParamID& id, int32& index) override {
        for (int32 i = 0; i < used_; ++i)
            if (queues_[i].getParameterId() == id) {
                index = i;
                return &queues_[i];
            }
        if (used_ >= kMaxQueues) return nullptr;
        queues_[used_].reset(id);
        index = used_;
        return &queues_[used_++];
    }
    bool set(V::ParamID id, V::ParamValue value) noexcept {
        int32 index = 0;
        ParamQueue* q = static_cast<ParamQueue*>(addParameterData(id, index));
        if (!q) return false;
        q->set_first(value);
        return true;
    }
    void clear() noexcept { used_ = 0; }

private:
    ParamQueue queues_[kMaxQueues];
    int32 used_ = 0;
};

// No events in (effects only, for now); events out are dropped.
class EventList final : public V::IEventList {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (iid_is(iid, Steinberg::FUnknown_iid) || iid_is(iid, V::IEventList_iid)) {
            *obj = this;
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    SONORA_UNCOUNTED
    int32 PLUGIN_API getEventCount() override { return 0; }
    tresult PLUGIN_API getEvent(int32, V::Event&) override { return Steinberg::kResultFalse; }
    tresult PLUGIN_API addEvent(V::Event&) override { return Steinberg::kResultOk; }
};

#undef SONORA_UNCOUNTED

template <class T>
T* query(FUnknown* from, const TUID iid) {
    void* p = nullptr;
    if (!from || from->queryInterface(iid, &p) != Steinberg::kResultOk) return nullptr;
    return static_cast<T*>(p);
}

}  // namespace

// ---------------------------------------------------------------------------
// Modules

class Vst3HostContext {
public:
    HostApplication app;
};

struct Vst3Module {
    struct Class {
        char cid[16];
        std::string id, name, category, sub_categories;
    };
    std::string bundle;
    void* library = nullptr;
    Steinberg::IPluginFactory* factory = nullptr;
    bool (*exit_fn)() = nullptr;
    std::vector<Class> classes;

    ~Vst3Module() {
        if (factory) factory->release();
        if (exit_fn) exit_fn();
        close_library(library);
    }
};

Vst3World::Vst3World() : context_(std::make_unique<Vst3HostContext>()) {}

// Processors are gone by now (the chain is destroyed first); modules unload
// in reverse load order.
Vst3World::~Vst3World() {
    while (!modules_.empty()) modules_.pop_back();
}

bool Vst3World::is_vst3_id(const std::string& id) { return id.rfind(kPrefix, 0) == 0; }

void Vst3World::add_bundle(const std::string& path) { explicit_.push_back(path); }

std::vector<std::string> Vst3World::bundles() const {
    std::vector<std::string> out = explicit_;
    for (const std::string& root : vst3_default_roots()) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;
        std::vector<std::string> found;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            const fs::path& p = it->path();
            if (p.extension() != ".vst3") continue;
            found.push_back(p.string());
            // A bundle is a leaf: plugins do not nest bundles.
            if (it->is_directory(ec)) it.disable_recursion_pending();
        }
        std::sort(found.begin(), found.end());
        out.insert(out.end(), found.begin(), found.end());
    }
    return out;
}

Vst3Module* Vst3World::load(const std::string& bundle, std::string& error) {
    for (auto& m : modules_)
        if (m->bundle == bundle) return m.get();
    std::error_code ec;
    const fs::path b = fs::absolute(bundle, ec);
    std::string binary;
    if (fs::is_directory(b, ec)) {
        binary = (b / "Contents" / vst3_architecture() / (b.stem().string() + ".so")).string();
    } else if (fs::is_regular_file(b, ec)) {
        binary = b.string();  // the pre-3.6.10 single-file layout
    } else {
        error = "no VST3 bundle at " + bundle;
        return nullptr;
    }
    auto m = std::make_unique<Vst3Module>();
    m->bundle = bundle;
    m->library = open_library(binary, error);
    if (!m->library) return nullptr;
    using Entry = bool (*)(void*);
    using GetFactory = Steinberg::IPluginFactory* (*)();
    if (auto entry = reinterpret_cast<Entry>(library_symbol(m->library, "ModuleEntry"))) {
        if (!entry(m->library)) {
            error = bundle + ": ModuleEntry failed";
            return nullptr;
        }
        m->exit_fn = reinterpret_cast<bool (*)()>(library_symbol(m->library, "ModuleExit"));
    }
    auto get = reinterpret_cast<GetFactory>(library_symbol(m->library, "GetPluginFactory"));
    if (!get || !(m->factory = get())) {
        error = bundle + ": no plugin factory";
        return nullptr;
    }
    auto* f2 = query<Steinberg::IPluginFactory2>(m->factory, Steinberg::IPluginFactory2_iid);
    const int32 n = m->factory->countClasses();
    for (int32 i = 0; i < n; ++i) {
        Steinberg::PClassInfo ci{};
        if (m->factory->getClassInfo(i, &ci) != Steinberg::kResultOk) continue;
        Vst3Module::Class c;
        std::memcpy(c.cid, ci.cid, sizeof c.cid);
        c.id = kPrefix + hex_of(ci.cid);
        c.name = cstr(ci.name, sizeof ci.name);
        c.category = cstr(ci.category, sizeof ci.category);
        Steinberg::PClassInfo2 ci2{};
        if (f2 && f2->getClassInfo2(i, &ci2) == Steinberg::kResultOk)
            c.sub_categories = cstr(ci2.subCategories, sizeof ci2.subCategories);
        m->classes.push_back(std::move(c));
    }
    if (f2) f2->release();
    modules_.push_back(std::move(m));
    return modules_.back().get();
}

Vst3Module* Vst3World::module_for(const std::string& id, int& class_index, ChainError& error) {
    TUID cid;
    if (!is_vst3_id(id) || !parse_hex(id.substr(std::strlen(kPrefix)), cid)) {
        error.reason = Refusal::NotFound;
        error.message = "not a VST3 class id (want vst3: and 32 hex digits): " + id;
        return nullptr;
    }
    const std::string want = upper(id);
    auto has = [&](Vst3Module* m) {
        for (std::size_t i = 0; i < m->classes.size(); ++i)
            if (upper(m->classes[i].id) == want && m->classes[i].category == kVstAudioEffectClass) {
                class_index = static_cast<int>(i);
                return true;
            }
        return false;
    };
    for (auto& m : modules_)
        if (has(m.get())) return m.get();

    // Bundles whose moduleinfo.json (VST 3.7.5 and later) names the class
    // first, then those without one. A bundle whose moduleinfo.json does not
    // name it is not loaded: no code of an unrelated plugin runs here.
    const std::string hex = want.substr(std::strlen(kPrefix));
    std::vector<std::string> unknown;
    std::string last_error;
    for (const std::string& b : bundles()) {
        std::ifstream f(fs::path(b) / "Contents" / "Resources" / "moduleinfo.json");
        if (!f) {
            unknown.push_back(b);
            continue;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        if (upper(ss.str()).find(hex) == std::string::npos) continue;
        if (Vst3Module* m = load(b, last_error); m && has(m)) return m;
    }
    for (const std::string& b : unknown)
        if (Vst3Module* m = load(b, last_error); m && has(m)) return m;
    error.reason = Refusal::NotFound;
    error.message = "no VST3 effect class " + id + " is installed" +
                    (last_error.empty() ? "" : " (last load error: " + last_error + ")");
    return nullptr;
}

// ---------------------------------------------------------------------------
// Processor

namespace {

class Vst3Processor final : public Processor {
public:
    Vst3Processor(Vst3HostContext& context, Vst3Module& module, const Vst3Module::Class& cls)
        : context_(context), module_(module), cls_(cls), handler_(latency_dirty_) {}

    ~Vst3Processor() override {
        for (Instance& i : inst_) {
            if (i.processing) i.processor->setProcessing(false);
            if (i.active) i.component->setActive(false);
        }
        if (comp_cp_ && ctrl_cp_) {
            comp_cp_->disconnect(ctrl_cp_);
            ctrl_cp_->disconnect(comp_cp_);
        }
        if (comp_cp_) comp_cp_->release();
        if (ctrl_cp_) ctrl_cp_->release();
        if (controller_) {
            controller_->setComponentHandler(nullptr);
            if (own_controller_) controller_->terminate();
            controller_->release();
        }
        for (Instance& i : inst_) {
            if (i.processor) i.processor->release();
            if (i.component) {
                i.component->terminate();
                i.component->release();
            }
        }
    }

    // Create the first instance and its controller, and describe the class:
    // buses at their default arrangement, parameters from the controller.
    bool open(ChainError& error) {
        info_.format = "vst3";
        info_.uri = cls_.id;
        info_.name = cls_.name;
        info_.class_label = cls_.sub_categories.empty() ? cls_.category : cls_.sub_categories;
        who_ = info_.name + " <" + info_.uri + ">";
        if (!add_instance(error)) return false;
        open_controller();

        V::IComponent* c = inst_[0].component;
        n_in_ = c->getBusCount(V::kAudio, V::kInput);
        n_out_ = c->getBusCount(V::kAudio, V::kOutput);
        for (int dir = 0; dir < 2; ++dir) {
            const auto d = dir == 0 ? V::kInput : V::kOutput;
            const int32 n = dir == 0 ? n_in_ : n_out_;
            for (int32 b = 0; b < n; ++b) {
                V::BusInfo bi{};
                c->getBusInfo(V::kAudio, d, b, bi);
                V::SpeakerArrangement a = 0;
                inst_[0].processor->getBusArrangement(d, b, a);
                PortInfo p;
                p.index = static_cast<uint32_t>(b);
                p.symbol = p.name = utf8(bi.name, 128);
                p.kind = PortKind::Audio;
                p.input = dir == 0;
                p.optional = bi.busType != V::kMain;
                p.channels = channels_of(a);
                if (b == 0) (dir == 0 ? info_.audio_in : info_.audio_out) = p.channels;
                info_.ports.push_back(std::move(p));
            }
        }
        if (n_in_ < 1 || n_out_ < 1 || !main_bus(V::kInput) || !main_bus(V::kOutput)) {
            error.reason = Refusal::ChannelMismatch;
            error.message = who_ + " has no main audio input and output; this chain carries effects";
            return false;
        }
        if (controller_) {
            const int32 n = controller_->getParameterCount();
            for (int32 i = 0; i < n; ++i) {
                V::ParameterInfo pi{};
                if (controller_->getParameterInfo(i, pi) != Steinberg::kResultOk) continue;
                PortInfo p;
                p.index = pi.id;
                p.symbol = p.name = utf8(pi.title, 128);
                p.units = utf8(pi.units, 128);
                p.kind = PortKind::Control;
                p.input = !(pi.flags & V::ParameterInfo::kIsReadOnly);
                p.min = 0.f;
                p.max = 1.f;
                p.def = static_cast<float>(pi.defaultNormalizedValue);
                if (p.input) writable_.push_back(pi.id);
                info_.ports.push_back(std::move(p));
            }
            std::sort(writable_.begin(), writable_.end());
        }
        // The default arrangement, for a describe-only start(); negotiate()
        // replaces it with the one agreed.
        read_arrangements(inst_[0]);
        return true;
    }

    // Ask for `channels` on the main buses; failing that, mono replicated.
    // What the plugin says afterwards is read back and must match: a "yes"
    // that kept the old arrangement is a refusal, not a configuration.
    bool negotiate(uint32_t channels, ChainError& error) {
        if (negotiate_one(inst_[0], channels)) {
            replicas_ = 1;
            return true;
        }
        if (channels > 1 && negotiate_one(inst_[0], 1)) {
            replicas_ = channels;
            for (uint32_t r = 1; r < replicas_; ++r) {
                if (!add_instance(error)) return false;
                if (!negotiate_one(inst_.back(), 1)) break;
            }
            if (inst_.size() == replicas_) return true;
        }
        error.reason = Refusal::ChannelMismatch;
        error.message = who_ + " declares " + std::to_string(info_.audio_in) + " in / " +
                        std::to_string(info_.audio_out) +
                        " out and did not take the arrangement asked for; the chain carries " +
                        std::to_string(channels) + " channels";
        return false;
    }

    // Buffers, bus activation, setup, activation. `in` and `out` are null
    // for a describe-only open, which keeps the default arrangement.
    //
    // Channel pointers are laid out from the arrangement agreed in
    // negotiate(), never from a fresh answer: a plugin that now says 8 on a
    // 2-channel chain would have the host index past the chain's buffers.
    // A fresh answer that differs is refused.
    bool start(const ChainConfig& config, const std::vector<float*>* in,
               const std::vector<float*>* out, ChainError& error) {
        max_frames_ = config.max_frames;
        trap_.assign(config.max_frames, 0.f);
        float canary;
        std::memcpy(&canary, &kTrapCanary, sizeof canary);
        std::fill(trap_.begin(), trap_.end(), canary);
        ctx_.sampleRate = config.sample_rate;
        ctx_.tempo = 120.0;
        ctx_.timeSigNumerator = 4;
        ctx_.timeSigDenominator = 4;
        ctx_.state = V::ProcessContext::kPlaying | V::ProcessContext::kTempoValid |
                     V::ProcessContext::kTimeSigValid | V::ProcessContext::kContTimeValid |
                     V::ProcessContext::kProjectTimeMusicValid;
        rate_ = config.sample_rate;

        for (std::size_t r = 0; r < inst_.size(); ++r) {
            Instance& i = inst_[r];
            V::IComponent* c = i.component;
            // Only the main buses carry audio here. Aux buses stay inactive
            // and still get silent buffers: some plugins read them anyway.
            for (int32 b = 0; b < n_in_; ++b) c->activateBus(V::kAudio, V::kInput, b, b == 0);
            for (int32 b = 0; b < n_out_; ++b) c->activateBus(V::kAudio, V::kOutput, b, b == 0);
            for (int dir = 0; dir < 2; ++dir) {
                const auto d = dir == 0 ? V::kInput : V::kOutput;
                const int32 n = c->getBusCount(V::kEvent, d);
                for (int32 b = 0; b < n; ++b) c->activateBus(V::kEvent, d, b, false);
            }
            for (int dir = 0; dir < 2; ++dir) {
                const auto d = dir == 0 ? V::kInput : V::kOutput;
                const int32 n = dir == 0 ? n_in_ : n_out_;
                auto& buses = dir == 0 ? i.in_bus : i.out_bus;
                auto& ptrs = dir == 0 ? i.in_ptrs : i.out_ptrs;
                buses.assign(static_cast<std::size_t>(n), V::AudioBusBuffers{});
                ptrs.assign(static_cast<std::size_t>(n), {});
                const auto& agreed = dir == 0 ? i.in_arr : i.out_arr;
                const std::vector<float*>* chain = dir == 0 ? in : out;
                for (int32 b = 0; b < n; ++b) {
                    V::SpeakerArrangement now = 0;
                    i.processor->getBusArrangement(d, b, now);
                    const V::SpeakerArrangement a = agreed[static_cast<std::size_t>(b)];
                    const uint32_t ch = channels_of(a);
                    const uint32_t want = !chain ? ch : replicas_ > 1 ? 1u : uint32_t(chain->size());
                    if (now != a || (b == 0 && ch != want)) {
                        error.reason = Refusal::ChannelMismatch;
                        error.message = who_ + " agreed to " + std::to_string(ch) + " channel(s) on its " +
                                        (dir == 0 ? "input" : "output") + " bus " + std::to_string(b) +
                                        " and now answers " + std::to_string(channels_of(now));
                        return false;
                    }
                    auto& p = ptrs[static_cast<std::size_t>(b)];
                    for (uint32_t k = 0; k < ch; ++k) {
                        if (b == 0 && chain) {
                            p.push_back((*chain)[replicas_ > 1 ? r : k]);
                        } else {
                            auto& pool = dir == 0 ? aux_in_ : aux_out_;
                            pool.emplace_back(config.max_frames, 0.f);
                            p.push_back(pool.back().data());
                        }
                    }
                    for (int32 k = 0; k < kSpareChannels; ++k) p.push_back(trap_.data());
                    buses[static_cast<std::size_t>(b)].numChannels = static_cast<int32>(ch);
                    buses[static_cast<std::size_t>(b)].channelBuffers32 = p.data();
                }
            }
            V::ProcessSetup setup{V::kRealtime, V::kSample32, static_cast<int32>(config.max_frames),
                                  static_cast<double>(config.sample_rate)};
            if (i.processor->setupProcessing(setup) != Steinberg::kResultOk) {
                error.reason = Refusal::InstantiateFailed;
                error.message = who_ + " refused processing at " + std::to_string(config.sample_rate) +
                                " Hz, " + std::to_string(config.max_frames) + " frames, 32-bit float";
                return false;
            }
            if (c->setActive(true) != Steinberg::kResultOk) {
                error.reason = Refusal::InstantiateFailed;
                error.message = who_ + " failed to activate";
                return false;
            }
            i.active = true;
            V::ProcessData& d = i.data;
            d.processMode = V::kRealtime;
            d.symbolicSampleSize = V::kSample32;
            d.numInputs = n_in_;
            d.numOutputs = n_out_;
            d.inputs = i.in_bus.data();
            d.outputs = i.out_bus.data();
            d.inputParameterChanges = &in_changes_;
            d.outputParameterChanges = &out_changes_;
            d.inputEvents = &in_events_;
            d.outputEvents = &out_events_;
            d.processContext = &ctx_;
        }
        latency_.store(inst_[0].processor->getLatencySamples(), std::memory_order_release);
        info_.latency_frames = latency();
        return true;
    }

    // setProcessing is the last step before the first process() (VST3
    // workflow). kNotImplemented is a legal answer.
    void begin_processing() {
        for (Instance& i : inst_) {
            i.processor->setProcessing(true);
            i.processing = true;
        }
    }

    uint32_t replicas() const noexcept override { return static_cast<uint32_t>(inst_.size()); }

    void run(uint32_t frames) noexcept override {
        const auto n = static_cast<int32>(std::min(frames, max_frames_));
        for (Instance& i : inst_) {
            for (std::size_t b = 1; b < i.in_bus.size(); ++b)
                for (int32 k = 0; k < i.in_bus[b].numChannels; ++k)
                    std::memset(i.in_ptrs[b][static_cast<std::size_t>(k)], 0, std::size_t(n) * sizeof(float));
            for (auto& b : i.in_bus) b.silenceFlags = 0;
            for (auto& b : i.out_bus) b.silenceFlags = 0;
            i.data.numSamples = n;
            i.processor->process(i.data);
            // A plugin may flag a channel silent instead of writing it; the
            // next plugin must not see the previous block's samples there.
            const V::AudioBusBuffers& main = i.out_bus[0];
            for (int32 k = 0; k < main.numChannels && k < 64; ++k)
                if (main.silenceFlags & (uint64_t(1) << k))
                    std::memset(main.channelBuffers32[k], 0, std::size_t(n) * sizeof(float));
            out_changes_.clear();
        }
        in_changes_.clear();
        ctx_.projectTimeSamples += n;
        ctx_.continousTimeSamples += n;
        ctx_.projectTimeMusic = double(ctx_.projectTimeSamples) / rate_ * (ctx_.tempo / 60.0);
    }

    // restartComponent(kLatencyChanged) came in, from whatever thread the
    // plugin chose. getLatencySamples() is a main-thread call in VST3, so it
    // is read here, on the thread that created the plugin, while process()
    // keeps running on its own thread; the value reaches the engine through
    // the next block's publish. The workflow's deactivate/reactivate around
    // the read is not done (brain/research/plugin-host-vst3.md, Finding 4).
    void control() override {
        if (latency_dirty_.exchange(false, std::memory_order_acq_rel))
            latency_.store(inst_[0].processor->getLatencySamples(), std::memory_order_release);
    }

    bool intact() const noexcept override {
        const std::size_t words = std::min<std::size_t>(kTrapCheckWords, trap_.size());
        for (std::size_t k = 0; k < words; ++k) {
            uint32_t bits;
            std::memcpy(&bits, &trap_[k], sizeof bits);
            if (bits != kTrapCanary) return false;
        }
        return true;
    }

    uint32_t latency() const noexcept override { return latency_.load(std::memory_order_acquire); }

    // Some plugins only know their latency once they have processed (LSP's
    // getLatencySamples() is 0 after setActive and 240 after one block), so
    // the value read at activation is not the one the engine plans with.
    void validated() override {
        latency_dirty_.store(false, std::memory_order_relaxed);
        latency_.store(inst_[0].processor->getLatencySamples(), std::memory_order_release);
        info_.latency_frames = latency();
    }

    // --list: a few blocks of silence through the default arrangement, so
    // the latency printed is the one the plugin settles on.
    void settle(uint32_t block_frames) {
        begin_processing();
        for (int k = 0; k < 4; ++k) run(block_frames);
        validated();
    }

    bool set_param(uint32_t id, float value) noexcept override {
        if (!std::binary_search(writable_.begin(), writable_.end(), id)) return false;
        if (!std::isfinite(value)) return false;
        return in_changes_.set(id, std::clamp(static_cast<double>(value), 0.0, 1.0));
    }

    bool param_id(const std::string& symbol, uint32_t& id) const override {
        if (!symbol.empty() && symbol[0] == '#') {
            id = static_cast<uint32_t>(std::strtoul(symbol.c_str() + 1, nullptr, 10));
            return std::binary_search(writable_.begin(), writable_.end(), id);
        }
        for (const PortInfo& p : info_.ports)
            if (p.kind == PortKind::Control && p.input && p.symbol == symbol) {
                id = p.index;
                return true;
            }
        return false;
    }

private:
    struct Instance {
        V::IComponent* component = nullptr;
        V::IAudioProcessor* processor = nullptr;
        bool active = false, processing = false;
        std::vector<V::AudioBusBuffers> in_bus, out_bus;
        // Per bus: the declared channels, then kSpareChannels trap pointers.
        std::vector<std::vector<float*>> in_ptrs, out_ptrs;
        // Per bus: the arrangement read back after negotiation.
        std::vector<V::SpeakerArrangement> in_arr, out_arr;
        V::ProcessData data{};
    };

    bool main_bus(V::BusDirection d) {
        V::BusInfo bi{};
        return inst_[0].component->getBusInfo(V::kAudio, d, 0, bi) == Steinberg::kResultOk &&
               bi.busType == V::kMain;
    }

    bool add_instance(ChainError& error) {
        void* obj = nullptr;
        if (module_.factory->createInstance(cls_.cid, V::IComponent_iid, &obj) != Steinberg::kResultOk ||
            !obj) {
            error.reason = Refusal::InstantiateFailed;
            error.message = who_ + " failed to instantiate";
            return false;
        }
        Instance i;
        i.component = static_cast<V::IComponent*>(obj);
        if (i.component->initialize(&context_.app) != Steinberg::kResultOk) {
            i.component->release();
            error.reason = Refusal::InstantiateFailed;
            error.message = who_ + " failed to initialise";
            return false;
        }
        i.processor = query<V::IAudioProcessor>(i.component, V::IAudioProcessor_iid);
        if (!i.processor) {
            i.component->terminate();
            i.component->release();
            error.reason = Refusal::UnsupportedFeature;
            error.message = who_ + " is not an IAudioProcessor";
            return false;
        }
        inst_.push_back(i);
        if (i.processor->canProcessSampleSize(V::kSample32) != Steinberg::kResultOk) {
            error.reason = Refusal::UnsupportedFeature;
            error.message = who_ + " cannot process 32-bit float";
            return false;
        }
        return true;
    }

    // Single-component plugins are their own controller; otherwise the
    // controller class is created, connected and told the component's state.
    // A plugin without a controller still runs; it has no parameters here.
    void open_controller() {
        V::IComponent* c = inst_[0].component;
        controller_ = query<V::IEditController>(c, V::IEditController_iid);
        if (!controller_) {
            TUID ccid{};
            void* obj = nullptr;
            if (c->getControllerClassId(ccid) != Steinberg::kResultOk) return;
            if (module_.factory->createInstance(ccid, V::IEditController_iid, &obj) != Steinberg::kResultOk ||
                !obj)
                return;
            controller_ = static_cast<V::IEditController*>(obj);
            if (controller_->initialize(&context_.app) != Steinberg::kResultOk) {
                controller_->release();
                controller_ = nullptr;
                return;
            }
            own_controller_ = true;
            comp_cp_ = query<V::IConnectionPoint>(c, V::IConnectionPoint_iid);
            ctrl_cp_ = query<V::IConnectionPoint>(controller_, V::IConnectionPoint_iid);
            if (comp_cp_ && ctrl_cp_) {
                comp_cp_->connect(ctrl_cp_);
                ctrl_cp_->connect(comp_cp_);
            }
            MemoryStream state;
            if (c->getState(&state) == Steinberg::kResultOk) {
                state.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
                controller_->setComponentState(&state);
            }
        }
        controller_->setComponentHandler(&handler_);
    }

    bool negotiate_one(Instance& i, uint32_t channels) {
        const V::SpeakerArrangement want = arrangement_for(channels);
        if (!want) return false;
        std::vector<V::SpeakerArrangement> ins(static_cast<std::size_t>(n_in_)),
            outs(static_cast<std::size_t>(n_out_));
        for (int32 b = 0; b < n_in_; ++b) i.processor->getBusArrangement(V::kInput, b, ins[std::size_t(b)]);
        for (int32 b = 0; b < n_out_; ++b) i.processor->getBusArrangement(V::kOutput, b, outs[std::size_t(b)]);
        ins[0] = want;
        outs[0] = want;
        i.processor->setBusArrangements(ins.data(), n_in_, outs.data(), n_out_);
        read_arrangements(i);
        return i.in_arr[0] == want && i.out_arr[0] == want;
    }

    void read_arrangements(Instance& i) {
        i.in_arr.assign(static_cast<std::size_t>(n_in_), 0);
        i.out_arr.assign(static_cast<std::size_t>(n_out_), 0);
        for (int32 b = 0; b < n_in_; ++b) i.processor->getBusArrangement(V::kInput, b, i.in_arr[std::size_t(b)]);
        for (int32 b = 0; b < n_out_; ++b)
            i.processor->getBusArrangement(V::kOutput, b, i.out_arr[std::size_t(b)]);
    }

    Vst3HostContext& context_;
    Vst3Module& module_;
    const Vst3Module::Class& cls_;
    std::string who_;
    std::vector<Instance> inst_;
    V::IEditController* controller_ = nullptr;
    bool own_controller_ = false;
    V::IConnectionPoint* comp_cp_ = nullptr;
    V::IConnectionPoint* ctrl_cp_ = nullptr;
    std::atomic<bool> latency_dirty_{false};
    ComponentHandler handler_;
    ParamChanges in_changes_, out_changes_;
    EventList in_events_, out_events_;
    V::ProcessContext ctx_{};
    double rate_ = 48000.0;
    int32 n_in_ = 0, n_out_ = 0;
    uint32_t replicas_ = 1, max_frames_ = 0;
    std::vector<std::vector<float>> aux_in_, aux_out_;
    std::vector<float> trap_;
    std::vector<uint32_t> writable_;  // sorted ParamIDs the engine may set
    std::atomic<uint32_t> latency_{0};  // written by the main thread, read by the processing one
};

}  // namespace

std::unique_ptr<Processor> Vst3World::instantiate(const std::string& id, const ChainConfig& config,
                                                  const std::vector<float*>& in,
                                                  const std::vector<float*>& out,
                                                  ChainError& error) {
    int ci = -1;
    Vst3Module* m = module_for(id, ci, error);
    if (!m) return nullptr;
    auto p = std::make_unique<Vst3Processor>(*context_, *m, m->classes[std::size_t(ci)]);
    if (!p->open(error) || !p->negotiate(config.channels, error) || !p->start(config, &in, &out, error))
        return nullptr;
    p->begin_processing();
    return p;
}

bool Vst3World::describe(const std::string& id, PluginInfo& out, ChainError& error) {
    int ci = -1;
    Vst3Module* m = module_for(id, ci, error);
    if (!m) return false;
    Vst3Processor p(*context_, *m, m->classes[std::size_t(ci)]);
    ChainConfig cfg;
    if (!p.open(error) || !p.start(cfg, nullptr, nullptr, error)) return false;
    p.settle(cfg.block_frames);
    out = p.info();
    return true;
}

bool Vst3World::describe_bundle(const std::string& bundle,
                                const std::function<void(const PluginInfo&)>& each, std::string& error) {
    Vst3Module* m = load(bundle, error);
    if (!m) return false;
    for (const Vst3Module::Class& c : m->classes) {
        if (c.category != kVstAudioEffectClass) continue;
        PluginInfo info;
        {
            Vst3Processor p(*context_, *m, c);
            ChainError err;
            ChainConfig cfg;
            if (p.open(err) && p.start(cfg, nullptr, nullptr, err)) {
                p.settle(cfg.block_frames);
                info = p.info();
            } else {
                info = p.info();
                info.format = "vst3";
                info.uri = c.id;
                info.name = c.name;
                info.missing_features.push_back(err.message);
            }
        }
        each(info);
    }
    return true;
}

}  // namespace sonora::plugin_host
