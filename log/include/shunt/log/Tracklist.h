// Tracklist logging (RS-04): events from status packets, "played" rows and exporters.
#pragma once
#include "shunt/net/Events.h"
#include <cstdint>
#include <string>
#include <vector>

namespace shunt::log {

struct TrackEvent {
    enum Kind : uint8_t { Loaded, OnAir, OffAir, Unloaded, Master } kind;
    int64_t wallMs;
    uint8_t deck;
    uint32_t rekordboxId;
    uint8_t slot, trackType;
    uint16_t trackNumber;
};

struct Row {
    int64_t startedAtMs = 0, endedAtMs = 0;
    uint8_t deck = 0, slot = 0;
    uint32_t rekordboxId = 0;
    uint16_t trackNumber = 0;
    std::string title, artist, album, genre, key;
    double bpm = 0;
    double durationS = 0;
    double playedPct = 0;
    std::string source = "status";
};

const char* eventKindName(TrackEvent::Kind k);

class TracklistSession {
public:
    static constexpr int64_t kPlayedMs = 30'000;
    static constexpr double kPlayedPct = 20.0;

    void onStatus(uint8_t deck, const net::PlayerStatusEvent& s, int64_t wallMs);
    void onOnAir(const net::OnAirEvent& o, int64_t wallMs);
    void finish(int64_t wallMs);
    void setDuration(uint8_t deck, double seconds);

    const std::vector<TrackEvent>& events() const { return events_; }
    const std::vector<Row>& rows() const { return rows_; }

private:
    struct DeckState {
        bool loaded = false;
        uint32_t rekordboxId = 0;
        uint8_t slot = 0, trackType = 0;
        uint16_t trackNumber = 0;
        bool onAir = false, master = false;
        int64_t activeSinceMs = -1;
        int64_t firstActiveMs = -1, lastActiveMs = -1;
        int64_t activeMs = 0;
        double durationS = 0;
    };
    void setActive(uint8_t deck, DeckState& d, bool active, int64_t wallMs);
    void closeRow(uint8_t deck, DeckState& d, int64_t wallMs);
    DeckState decks_[256];
    std::vector<TrackEvent> events_;
    std::vector<Row> rows_;
};

std::string toCsv(const std::vector<Row>& rows);
std::string toCue(const std::vector<Row>& rows, int64_t setStartMs, const std::string& performer,
                  const std::string& title, const std::string& file = "mix.wav");
std::string toTimestampText(const std::vector<Row>& rows, int64_t setStartMs);
std::string toNdjson(const std::vector<TrackEvent>& events, const std::vector<Row>& rows);
std::string toPerformanceReportCsv(const std::vector<Row>& rows, const std::string& date, const std::string& venue);

} // namespace shunt::log
