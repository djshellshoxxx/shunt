// Track metadata store (FS-00 section 4). Original Shunt code.
#include "shunt/app/MetaStore.h"
#include "shunt/app/Json.h"
#include <fstream>
#include <sstream>

namespace shunt::app {

std::optional<log::TrackMeta> MetaStore::get(uint32_t id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(id);
    if (it == map_.end()) return std::nullopt;
    return it->second;
}

bool MetaStore::contains(uint32_t id) const {
    std::lock_guard<std::mutex> lk(mu_);
    return map_.count(id) != 0;
}

size_t MetaStore::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return map_.size();
}

log::MetaMap MetaStore::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return map_;
}

void MetaStore::replace(log::MetaMap m) {
    std::lock_guard<std::mutex> lk(mu_);
    map_ = std::move(m);
}

void MetaStore::set(uint32_t id, const log::TrackMeta& t) {
    std::lock_guard<std::mutex> lk(mu_);
    map_[id] = t;
}

void MetaStore::apply(std::vector<log::Row>& rows) const {
    std::lock_guard<std::mutex> lk(mu_);
    log::applyMeta(rows, map_);
}

bool MetaStore::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    Json j;
    if (!Json::parse(ss.str(), j) || !j.isArray()) return false;
    log::MetaMap m;
    for (auto& o : j.items()) {
        log::TrackMeta t;
        t.title = o.get("title").asString(""); t.artist = o.get("artist").asString(""); t.album = o.get("album").asString("");
        t.genre = o.get("genre").asString(""); t.key = o.get("key").asString("");
        t.bpm = o.get("bpm").asNumber(); t.durationS = o.get("durationS").asNumber();
        m[uint32_t(o.get("id").asNumber())] = t;
    }
    replace(std::move(m));
    return true;
}

bool MetaStore::save(const std::string& path) const {
    Json arr = Json::array();
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& kv : map_) {
            Json o = Json::object();
            o["id"] = int64_t(kv.first); o["title"] = kv.second.title; o["artist"] = kv.second.artist; o["album"] = kv.second.album;
            o["genre"] = kv.second.genre; o["key"] = kv.second.key; o["bpm"] = kv.second.bpm; o["durationS"] = kv.second.durationS;
            arr.push(o);
        }
    }
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << arr.dump();
    return bool(f);
}

} // namespace shunt::app
