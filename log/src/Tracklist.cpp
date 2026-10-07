#include "shunt/log/Tracklist.h"
#include <cstdio>
#include <sstream>
#include <ctime>

namespace shunt::log {

const char* eventKindName(TrackEvent::Kind k) {
    switch (k) {
    case TrackEvent::Loaded: return "loaded";
    case TrackEvent::OnAir: return "onair";
    case TrackEvent::OffAir: return "offair";
    case TrackEvent::Unloaded: return "unloaded";
    case TrackEvent::Master: return "master";
    }
    return "?";
}

void TracklistSession::setDuration(uint8_t deck, double seconds) { decks_[deck].durationS = seconds; }

void TracklistSession::setActive(uint8_t deck, DeckState& d, bool active, int64_t wallMs) {
    (void)deck;
    const bool was = d.activeSinceMs >= 0;
    if (active && !was) {
        d.activeSinceMs = wallMs;
        if (d.firstActiveMs < 0) d.firstActiveMs = wallMs;
    } else if (!active && was) {
        d.activeMs += wallMs - d.activeSinceMs;
        d.lastActiveMs = wallMs;
        d.activeSinceMs = -1;
    }
}

void TracklistSession::closeRow(uint8_t deck, DeckState& d, int64_t wallMs) {
    setActive(deck, d, false, wallMs);
    const bool byTime = d.activeMs >= kPlayedMs;
    const bool byPct = d.durationS > 0 && (d.activeMs / 1000.0) / d.durationS * 100.0 >= kPlayedPct;
    if (d.firstActiveMs >= 0 && (byTime || byPct)) {
        Row r;
        r.startedAtMs = d.firstActiveMs;
        r.endedAtMs = d.lastActiveMs >= 0 ? d.lastActiveMs : wallMs;
        r.deck = deck;
        r.slot = d.slot;
        r.rekordboxId = d.rekordboxId;
        r.trackNumber = d.trackNumber;
        r.durationS = d.durationS;
        r.playedPct = d.durationS > 0 ? (d.activeMs / 1000.0) / d.durationS * 100.0 : 0;
        rows_.push_back(r);
    }
    d.activeMs = 0;
    d.firstActiveMs = d.lastActiveMs = -1;
}

void TracklistSession::onStatus(uint8_t deck, const net::PlayerStatusEvent& s, int64_t wallMs) {
    DeckState& d = decks_[deck];
    const bool nowLoaded = s.rekordboxId != 0 || s.trackType != 0;
    if (d.loaded && (!nowLoaded || s.rekordboxId != d.rekordboxId || s.slot != d.slot)) {
        closeRow(deck, d, wallMs);
        events_.push_back({TrackEvent::Unloaded, wallMs, deck, d.rekordboxId, d.slot, d.trackType, d.trackNumber});
        d.loaded = false;
        d.onAir = d.master = false;
    }
    if (nowLoaded && !d.loaded) {
        d.loaded = true;
        d.rekordboxId = s.rekordboxId;
        d.slot = s.slot;
        d.trackType = s.trackType;
        d.trackNumber = s.trackNumber;
        d.onAir = d.master = false;
        events_.push_back({TrackEvent::Loaded, wallMs, deck, s.rekordboxId, s.slot, s.trackType, s.trackNumber});
    }
    if (!d.loaded) return;
    if (s.onAir != d.onAir) {
        d.onAir = s.onAir;
        events_.push_back({s.onAir ? TrackEvent::OnAir : TrackEvent::OffAir, wallMs, deck, d.rekordboxId, d.slot, d.trackType, d.trackNumber});
    }
    if (s.master && !d.master) {
        events_.push_back({TrackEvent::Master, wallMs, deck, d.rekordboxId, d.slot, d.trackType, d.trackNumber});
    }
    d.master = s.master;
    setActive(deck, d, d.onAir || d.master, wallMs);
}

void TracklistSession::onOnAir(const net::OnAirEvent& o, int64_t wallMs) {
    for (uint8_t ch = 0; ch < o.channelCount; ++ch) {
        DeckState& d = decks_[ch + 1];
        if (!d.loaded) continue;
        const bool on = o.channels[ch] != 0;
        if (on != d.onAir) {
            d.onAir = on;
            events_.push_back({on ? TrackEvent::OnAir : TrackEvent::OffAir, wallMs, uint8_t(ch + 1), d.rekordboxId, d.slot, d.trackType, d.trackNumber});
            setActive(uint8_t(ch + 1), d, d.onAir || d.master, wallMs);
        }
    }
}

void TracklistSession::finish(int64_t wallMs) {
    for (int i = 0; i < 256; ++i)
        if (decks_[i].loaded) closeRow(uint8_t(i), decks_[i], wallMs);
}

// ---------------------------------------------------------------- exporters

namespace {

std::string csvField(const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) { if (c == '"') out += '"'; out += c; }
    return out + "\"";
}

std::string hms(int64_t ms) {
    if (ms < 0) ms = 0;
    const int64_t s = ms / 1000;
    char b[48];
    std::snprintf(b, sizeof b, "%02lld:%02lld:%02lld", (long long)(s / 3600), (long long)((s / 60) % 60), (long long)(s % 60));
    return b;
}

std::string isoTime(int64_t wallMs) {
    const time_t t = time_t(wallMs / 1000);
    tm tmv{};
    gmtime_r(&t, &tmv);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return b;
}

std::string jsonStr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else o += c;
    }
    return o + "\"";
}

std::string orId(const std::string& s) { return s.empty() ? "ID" : s; }

} // namespace

std::string toCsv(const std::vector<Row>& rows) {
    std::string out = "#,name,artist,album,genre,bpm,key,start time,end time,played,deck,rekordbox_id,source\n";
    int n = 0;
    for (const auto& r : rows) {
        char bpm[16]; std::snprintf(bpm, sizeof bpm, "%.2f", r.bpm);
        char pct[16]; std::snprintf(pct, sizeof pct, "%.0f%%", r.playedPct);
        out += std::to_string(++n) + "," + csvField(r.title) + "," + csvField(r.artist) + "," + csvField(r.album) + "," +
               csvField(r.genre) + "," + (r.bpm > 0 ? bpm : "") + "," + csvField(r.key) + "," + isoTime(r.startedAtMs) + "," +
               isoTime(r.endedAtMs) + "," + (r.durationS > 0 ? pct : "") + "," + std::to_string(r.deck) + "," +
               std::to_string(r.rekordboxId) + "," + csvField(r.source) + "\n";
    }
    return out;
}

std::string toCue(const std::vector<Row>& rows, int64_t setStartMs, const std::string& performer,
                  const std::string& title, const std::string& file) {
    std::string out = "PERFORMER \"" + performer + "\"\nTITLE \"" + title + "\"\nFILE \"" + file + "\" WAVE\n";
    int n = 0;
    for (const auto& r : rows) {
        int64_t rel = r.startedAtMs - setStartMs;
        if (rel < 0) rel = 0;
        const int64_t frames = (rel % 1000) * 75 / 1000;
        const int64_t secs = rel / 1000;
        char idx[32];
        std::snprintf(idx, sizeof idx, "%02lld:%02lld:%02lld", (long long)(secs / 60), (long long)(secs % 60), (long long)frames);
        char trk[16];
        std::snprintf(trk, sizeof trk, "%02d", ++n);
        out += "  TRACK " + std::string(trk) + " AUDIO\n    TITLE \"" + orId(r.title) + "\"\n    PERFORMER \"" +
               orId(r.artist) + "\"\n    INDEX 01 " + idx + "\n";
    }
    return out;
}

std::string toTimestampText(const std::vector<Row>& rows, int64_t setStartMs) {
    std::string out;
    for (const auto& r : rows)
        out += "[" + hms(r.startedAtMs - setStartMs) + "] " + orId(r.artist) + " - " + orId(r.title) + "\n";
    return out;
}

std::string toNdjson(const std::vector<TrackEvent>& events, const std::vector<Row>& rows) {
    std::string out;
    for (const auto& e : events) {
        out += "{\"type\":\"event\",\"kind\":\"" + std::string(eventKindName(e.kind)) + "\",\"t\":" + std::to_string(e.wallMs) +
               ",\"deck\":" + std::to_string(e.deck) + ",\"rekordboxId\":" + std::to_string(e.rekordboxId) +
               ",\"slot\":" + std::to_string(e.slot) + ",\"trackType\":" + std::to_string(e.trackType) +
               ",\"trackNumber\":" + std::to_string(e.trackNumber) + "}\n";
    }
    for (const auto& r : rows) {
        char num[64];
        std::snprintf(num, sizeof num, "%.2f,\"durationS\":%.1f,\"playedPct\":%.1f", r.bpm, r.durationS, r.playedPct);
        out += "{\"type\":\"row\",\"startedAt\":" + std::to_string(r.startedAtMs) + ",\"endedAt\":" + std::to_string(r.endedAtMs) +
               ",\"deck\":" + std::to_string(r.deck) + ",\"slot\":" + std::to_string(r.slot) +
               ",\"rekordboxId\":" + std::to_string(r.rekordboxId) + ",\"trackNumber\":" + std::to_string(r.trackNumber) +
               ",\"title\":" + jsonStr(r.title) + ",\"artist\":" + jsonStr(r.artist) + ",\"album\":" + jsonStr(r.album) +
               ",\"key\":" + jsonStr(r.key) + ",\"bpm\":" + num + ",\"source\":" + jsonStr(r.source) + "}\n";
    }
    return out;
}

std::string toPerformanceReportCsv(const std::vector<Row>& rows, const std::string& date, const std::string& venue) {
    std::string out = "date,venue,title,artist,writer(s),publisher,ISRC,duration\n";
    for (const auto& r : rows)
        out += csvField(date) + "," + csvField(venue) + "," + csvField(orId(r.title)) + "," + csvField(orId(r.artist)) +
               ",,,," + hms(r.endedAtMs - r.startedAtMs) + "\n";
    return out;
}

} // namespace shunt::log
