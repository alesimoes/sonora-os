// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where the operator's own API key lives (ADR-0155 §5). The interface hides
// the storage (a file readable by the engine's user only, on the appliance)
// so nothing else holds the key longer than one request. A key is never
// logged, never written into a request body and never shown back in full.
#pragma once

#include <string>

namespace sonora::assistant {

class KeyStore {
public:
    virtual ~KeyStore() = default;
    virtual bool has_key() const = 0;
    // False when there is no key or it cannot be read; `key` is left empty.
    virtual bool load(std::string& key) const = 0;
    // False, with `error`, when it cannot be stored. An empty key is refused:
    // forgetting the key is clear(), not save("").
    virtual bool save(const std::string& key, std::string& error) = 0;
    virtual void clear() = 0;
};

// Settings shows this, never the key: "sk-...7f3a".
inline std::string mask_key(const std::string& key) {
    if (key.size() <= 8) return std::string(key.empty() ? "" : "****");
    return key.substr(0, 3) + "..." + key.substr(key.size() - 4);
}

// For tests and for a session that should not persist the key.
class MemoryKeyStore final : public KeyStore {
public:
    bool has_key() const override { return !key_.empty(); }
    bool load(std::string& key) const override {
        key = key_;
        return !key_.empty();
    }
    bool save(const std::string& key, std::string& error) override {
        if (key.empty()) {
            error = "empty API key";
            return false;
        }
        key_ = key;
        return true;
    }
    void clear() override { key_.clear(); }

private:
    std::string key_;
};

}  // namespace sonora::assistant
