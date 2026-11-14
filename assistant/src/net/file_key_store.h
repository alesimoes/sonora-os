// SPDX-License-Identifier: GPL-3.0-or-later
//
// The operator's API key in one file on the persistence path (ADR-0155 §5),
// readable by the engine's user only. Never logs; errors name the path, not
// the key.
#pragma once

#include <string>
#include <utility>

#include "sonora/assistant/key_store.h"

namespace sonora::assistant {

class FileKeyStore final : public KeyStore {
public:
    explicit FileKeyStore(std::string path) : path_(std::move(path)) {}
    bool has_key() const override;
    bool load(std::string& key) const override;
    bool save(const std::string& key, std::string& error) override;
    void clear() override;

private:
    std::string path_;
};

}  // namespace sonora::assistant
