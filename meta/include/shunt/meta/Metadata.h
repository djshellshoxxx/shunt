// Track metadata resolution (RS-04 section "Metadata resolution").
// Written from publicly documented packet analysis; no code from EPL or GPL projects.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace shunt::meta {

struct TrackMetadata {
    uint32_t rekordboxId = 0;
    std::string title, artist, album, genre, key, label;
    double bpm = 0;
    double durationS = 0;
    std::string source;        // "dbserver", "xml"
};

// Path A: dbserver over TCP (player port 12523 discovery, then usually 1051).
// Only usable when Shunt holds a real player number (1 to 4, or 1 to 6 with
// CDJ-3000 / XDJ-AZ) and the queried player is a different number. Requests run
// on a worker thread; results are cached by (player ip, slot, rekordbox id).
class DbServerClient {
public:
    using Callback = std::function<void(bool ok, const TrackMetadata&)>;

    DbServerClient();
    ~DbServerClient();
    DbServerClient(const DbServerClient&) = delete;
    DbServerClient& operator=(const DbServerClient&) = delete;

    // Our own device number (1 to 6); 0 disables queries.
    void setOwnNumber(uint8_t number);
    // Queue a lookup. Callback runs on the worker thread.
    void request(uint32_t playerIp, uint8_t player, uint8_t slot, uint8_t trackType,
                 uint32_t rekordboxId, Callback cb);
    // For tests: override the discovery port (default 12523).
    void setDiscoveryPort(uint16_t port);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Path 2: the user's rekordbox XML export (COLLECTION/TRACK), matched by id.
class RekordboxXml {
public:
    bool load(const std::string& path, std::string* error = nullptr);
    bool loadFromString(const std::string& xml, std::string* error = nullptr);
    const TrackMetadata* find(uint32_t trackId) const;
    size_t size() const { return tracks_.size(); }

private:
    std::map<uint32_t, TrackMetadata> tracks_;
};

} // namespace shunt::meta
