#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sonora::smart_controls {

constexpr std::size_t kMaxMacros = 8;
constexpr int kSchema = 1;

struct Mapping {
    std::uint64_t insert = 0;
    std::string key;
    float lo = 0.0f;
    float hi = 1.0f;
    bool invert = false;
};

struct Macro {
    std::string name;
    std::vector<Mapping> mappings;
};

struct SmartControlSet {
    std::vector<Macro> macros;
};

// The parameter value a macro position gives one mapping.
float macro_target(const Mapping& m, float macro_value);

// Empty string when valid, else the reason.
std::string validate(const SmartControlSet& set);

// Adds a macro; false with a reason when it would make the set invalid.
bool add_macro(SmartControlSet& set, Macro macro, std::string& why);

// The gesture surface of the insert controller, as the core needs it.
class ParamSink {
public:
    virtual ~ParamSink() = default;
    virtual bool has_param(std::uint64_t insert, const std::string& key) const = 0;
    virtual void begin(std::uint64_t insert) = 0;
    virtual void set(std::uint64_t insert, const std::string& key, float value) = 0;
    virtual void end(std::uint64_t insert) = 0;
};

struct ApplyResult {
    int applied = 0;
    int skipped = 0;  // unresolved mappings, kept in the set
};

// One begin/set.../end per touched insert. False with a reason for a bad
// macro index or a value outside [0,1].
bool apply(const SmartControlSet& set, std::size_t macro, float value,
           ParamSink& sink, ApplyResult& result, std::string& why);

class SmartControlsStore {
public:
    virtual ~SmartControlsStore() = default;
    virtual bool load(SmartControlSet& out, std::string& why) = 0;
    virtual bool save(const SmartControlSet& set, std::string& why) = 0;
};

std::string to_json(const SmartControlSet& set);
bool from_json(const std::string& text, SmartControlSet& out, std::string& why);

class JsonFileStore : public SmartControlsStore {
public:
    explicit JsonFileStore(std::string path) : path_(std::move(path)) {}
    bool load(SmartControlSet& out, std::string& why) override;
    bool save(const SmartControlSet& set, std::string& why) override;

private:
    std::string path_;
};

class MemoryStore : public SmartControlsStore {
public:
    bool load(SmartControlSet& out, std::string& why) override;
    bool save(const SmartControlSet& set, std::string& why) override;
    std::string text;  // the stored document; tests may corrupt it
};

class FakeSink : public ParamSink {
public:
    struct Call {
        char kind;
        std::uint64_t insert;
        std::string key;
        float value;
    };
    std::vector<Call> calls;
    std::vector<std::pair<std::uint64_t, std::string>> known;
    bool has_param(std::uint64_t insert, const std::string& key) const override;
    void begin(std::uint64_t insert) override { calls.push_back({'b', insert, {}, 0}); }
    void set(std::uint64_t insert, const std::string& key, float v) override {
        calls.push_back({'s', insert, key, v});
    }
    void end(std::uint64_t insert) override { calls.push_back({'e', insert, {}, 0}); }
};

}  // namespace sonora::smart_controls
