// RS-04 tracklist rules and exporters.
#include "TestFramework.h"
#include "shunt/log/Tracklist.h"

using namespace shunt::log;

namespace {
shunt::net::PlayerStatusEvent status(uint32_t id, bool onAir, bool master, uint8_t slot = 3) {
    shunt::net::PlayerStatusEvent s{};
    s.rekordboxId = id; s.trackType = id ? 1 : 0; s.slot = slot; s.onAir = onAir; s.master = master; s.playing = true;
    return s;
}
}

TEST_CASE("tracklist: played rule 30 s / 20 percent and start at first on-air") {
    TracklistSession t;
    int64_t ms = 1'700'000'000'000LL;
    t.onStatus(1, status(100, false, false), ms);            // loaded, not on air
    t.onStatus(1, status(100, true, false), ms + 5'000);     // on air at +5 s
    t.onStatus(1, status(100, false, false), ms + 45'000);   // off air after 40 s
    t.onStatus(1, status(101, false, false), ms + 50'000);   // new track: short play
    t.onStatus(1, status(101, true, true), ms + 51'000);
    t.onStatus(1, status(0, false, false), ms + 60'000);     // unloaded after 9 s
    t.setDuration(2, 100.0);
    t.onStatus(2, status(200, true, false), ms);             // 25 s of a 100 s track => 25 percent
    t.onStatus(2, status(0, false, false), ms + 25'000);
    t.finish(ms + 70'000);
    REQUIRE(t.rows().size() == 2u);
    CHECK_EQ(t.rows()[0].rekordboxId, 100u);
    CHECK_EQ(t.rows()[0].startedAtMs, ms + 5'000);
    CHECK_EQ(t.rows()[0].endedAtMs, ms + 45'000);
    CHECK_EQ(t.rows()[1].rekordboxId, 200u);
    CHECK_NEAR(t.rows()[1].playedPct, 25.0, 0.01);
    size_t loaded = 0, onair = 0, master = 0, unloaded = 0;
    for (auto& e : t.events()) {
        if (e.kind == TrackEvent::Loaded) ++loaded;
        if (e.kind == TrackEvent::OnAir) ++onair;
        if (e.kind == TrackEvent::Master) ++master;
        if (e.kind == TrackEvent::Unloaded) ++unloaded;
    }
    CHECK_EQ(loaded, 3u); CHECK_EQ(onair, 3u); CHECK_EQ(master, 1u); CHECK_EQ(unloaded, 3u);
}

TEST_CASE("tracklist exporters") {
    std::vector<Row> rows(2);
    rows[0].startedAtMs = 1'700'000'000'000LL; rows[0].endedAtMs = rows[0].startedAtMs + 300'000;
    rows[0].title = "Track, One"; rows[0].artist = "Artist"; rows[0].bpm = 128; rows[0].deck = 1; rows[0].rekordboxId = 7;
    rows[1].startedAtMs = rows[0].startedAtMs + 272'500; rows[1].endedAtMs = rows[1].startedAtMs + 200'000;
    rows[1].deck = 2;
    const int64_t setStart = rows[0].startedAtMs;
    const std::string csv = toCsv(rows);
    CHECK(csv.rfind("#,name,artist,album,genre,bpm,key,start time,end time,played,deck,rekordbox_id,source\n", 0) == 0);
    CHECK(csv.find("\"Track, One\"") != std::string::npos);
    CHECK(csv.find("2023-11-14T22:13:20Z") != std::string::npos);
    const std::string cue = toCue(rows, setStart, "DJ", "Set");
    CHECK(cue.find("FILE \"mix.wav\" WAVE") != std::string::npos);
    CHECK(cue.find("TRACK 01 AUDIO") != std::string::npos);
    CHECK(cue.find("INDEX 01 00:00:00") != std::string::npos);
    CHECK(cue.find("INDEX 01 04:32:37") != std::string::npos);   // 272.5 s -> 4:32 + 37 frames
    CHECK(cue.find("TITLE \"ID\"") != std::string::npos);
    const std::string txt = toTimestampText(rows, setStart);
    CHECK(txt == "[00:00:00] Artist - Track, One\n[00:04:32] ID - ID\n");
    std::vector<TrackEvent> ev{{TrackEvent::Loaded, setStart, 1, 7, 3, 1, 0}};
    const std::string nd = toNdjson(ev, rows);
    CHECK(nd.find("{\"type\":\"event\",\"kind\":\"loaded\"") == 0);
    CHECK(nd.find("\"title\":\"Track, One\"") != std::string::npos);
    const std::string perf = toPerformanceReportCsv(rows, "2023-11-14", "Club");
    CHECK(perf.find("date,venue,title,artist,writer(s),publisher,ISRC,duration\n") == 0);
    CHECK(perf.find("2023-11-14,Club,\"Track, One\",Artist,,,,00:05:00\n") != std::string::npos);
}
