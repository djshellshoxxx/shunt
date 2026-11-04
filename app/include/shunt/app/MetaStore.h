// Track metadata store (FS-00 section 4): rekordbox XML import and dbserver lookups, moved out
// of Core. Own mutex, a leaf lock (safe to call with the Core lock held). Original Shunt code.
#pragma once
#include "shunt/log/RekordboxXml.h"
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace shunt::app {

class MetaStore {
public:
    std::optional<log::TrackMeta> get(uint32_t id) const;
    bool contains(uint32_t id) const;
    size_t size() const;
    log::MetaMap snapshot() const;               // copy of everything
    void replace(log::MetaMap m);
    void set(uint32_t id, const log::TrackMeta& t);
    void apply(std::vector<log::Row>& rows) const;   // log::applyMeta under the lock (no copy)
    // JSON array file [{id,title,artist,album,genre,key,bpm,durationS}] (format unchanged from RS-07).
    bool load(const std::string& path);              // replaces the content; false if missing or invalid
    bool save(const std::string& path) const;

private:
    mutable std::mutex mu_;
    log::MetaMap map_;
};

} // namespace shunt::app
