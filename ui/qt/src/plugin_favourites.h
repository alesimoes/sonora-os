// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plug-ins the operator marked as favourites (ADR-0065 §6): how this
// interface looks at the catalog, never what the engine may run, so they
// never reach the engine. Keyed by format and id, the way plugin.block names
// a plug-in ("lv2 <uri>", "vst3 <class id>"). A favourite that is not in the
// catalog is kept and shown nowhere until it comes back.
//
// Where they are kept is behind FavouritesStore, so a test hands in a fake.
// The file is its own, beside preferences.json: save_preferences() writes
// that file whole from the Preferences struct, and a key added to it from
// here would be dropped by the next change of edit mode.
#pragma once

#include <QString>
#include <set>
#include <string>

#include "sonora/api/messages.h"

namespace sonora::ui {

// "lv2 <uri>" or "vst3 <class id>": plugin.block's argument, and the key.
std::string plugin_key(api::PluginFormat format, const std::string& id);

class FavouritesStore {
public:
    virtual ~FavouritesStore() = default;
    // Nothing saved yet is an empty set and true. A store that cannot be
    // read says why and leaves `out` alone.
    virtual bool load(std::set<std::string>& out, std::string& why) = 0;
    virtual bool save(const std::set<std::string>& keys, std::string& why) = 0;
};

// $SONORA_PLUGIN_FAVOURITES_FILE, else plugin-favourites.json in the folder
// of default_preferences_file().
QString default_favourites_file();

// {"version": 1, "favourites": ["lv2 urn:...", ...]}. A file of another
// version, or an entry that is not a key, is refused rather than read as
// something else.
class FileFavouritesStore : public FavouritesStore {
public:
    explicit FileFavouritesStore(QString file) : file_(std::move(file)) {}
    bool load(std::set<std::string>& out, std::string& why) override;
    bool save(const std::set<std::string>& keys, std::string& why) override;
    const QString& file() const { return file_; }

private:
    QString file_;
};

class PluginFavourites {
public:
    explicit PluginFavourites(FavouritesStore& store) : store_(store) {}

    // After a failed load, set() refuses: saving would overwrite a file
    // that holds the operator's favourites but could not be read.
    bool load(std::string& why);
    bool contains(const std::string& key) const { return keys_.count(key) != 0; }
    // Marks or unmarks, and saves. When the save fails the mark is put back
    // and false returned with the reason: a star that shows but is not kept
    // would be gone at the next start without a word.
    bool set(const std::string& key, bool favourite, std::string& why);
    const std::set<std::string>& keys() const { return keys_; }

private:
    FavouritesStore& store_;
    std::set<std::string> keys_;
    std::string unreadable_;   // why the last load failed; empty when it did not
};

}  // namespace sonora::ui
