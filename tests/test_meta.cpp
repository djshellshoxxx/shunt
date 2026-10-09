// O-T7 OSC input decode, L-T1/L-T2 rekordbox XML metadata.
#include "TestFramework.h"
#include "shunt/log/RekordboxXml.h"
#include "shunt/out/Osc.h"

using namespace shunt;

TEST_CASE("O-T7 OSC decode round trip and malformed input") {
    auto b = out::oscMessage("/shunt/baroffset", {out::OscArg::Int(3)});
    out::OscMessage m;
    REQUIRE(out::oscParse(b.data(), b.size(), m));
    CHECK(m.address == "/shunt/baroffset");
    REQUIRE(m.args.size() == 1u);
    CHECK_EQ(m.args[0].i, 3);
    auto c = out::oscMessage("/x", {out::OscArg::Float(1.5f), out::OscArg::Str("hello")});
    REQUIRE(out::oscParse(c.data(), c.size(), m));
    CHECK_NEAR(double(m.args[0].f), 1.5, 1e-9);
    CHECK(m.args[1].s == "hello");
    CHECK(!out::oscParse(b.data(), b.size() - 2, m));          // truncated argument
    const uint8_t junk[] = {'h', 'i', 0, 0};
    CHECK(!out::oscParse(junk, sizeof junk, m));
    CHECK(!out::oscParse(b.data(), 3, m));
}

TEST_CASE("L-T1 rekordbox XML parse: entities, playlist refs, similar tags") {
    const std::string xml =
        "<?xml version=\"1.0\"?><DJ_PLAYLISTS><COLLECTION Entries=\"2\">"
        "<TRACK TrackID=\"1001\" Name=\"Rock &amp; Roll &#233;\" Artist=\"A &lt;B&gt;\" Album=\"Al\" Genre=\"House\" Tonality=\"8A\" AverageBpm=\"128.00\" TotalTime=\"301\">"
        "<TEMPO Inizio=\"0.0\" Bpm=\"128.00\"/></TRACK>"
        "<TRACK TrackID=\"1002\" Name='Single' Artist=\"\" TotalTime=\"10\"/>"
        "</COLLECTION><PLAYLISTS><NODE><TRACK Key=\"1001\"/></NODE></PLAYLISTS></DJ_PLAYLISTS>";
    auto m = log::parseRekordboxXml(xml);
    REQUIRE(m.size() == 2u);
    CHECK(m[1001].title == "Rock & Roll \xc3\xa9");
    CHECK(m[1001].artist == "A <B>");
    CHECK(m[1001].key == "8A");
    CHECK_NEAR(m[1001].bpm, 128.0, 1e-9);
    CHECK_NEAR(m[1001].durationS, 301.0, 1e-9);
    CHECK(m[1002].title == "Single");
    CHECK(log::parseRekordboxXml("<TRACKS><TRACK Key=\"5\"/></TRACKS>").empty());
    CHECK(log::parseRekordboxXml("not xml at all").empty());
    CHECK(log::parseRekordboxXml("<TRACK TrackID=\"5\" Name=\"unterminated").empty());
}

TEST_CASE("L-T2 metadata applied to rows and visible in exports") {
    std::vector<log::Row> rows(2);
    rows[0].rekordboxId = 1001; rows[0].startedAtMs = 1000; rows[0].endedAtMs = 61000; rows[0].deck = 1;
    rows[1].rekordboxId = 777;  rows[1].startedAtMs = 61000; rows[1].endedAtMs = 121000; rows[1].deck = 2;
    log::MetaMap meta;
    meta[1001].title = "Song"; meta[1001].artist = "Artist"; meta[1001].bpm = 126;
    log::applyMeta(rows, meta);
    CHECK(rows[0].title == "Song");
    CHECK(rows[0].source == "rekordbox-xml");
    CHECK(rows[1].title.empty());
    const std::string txt = log::toTimestampText(rows, 1000);
    CHECK(txt.find("[00:00:00] Artist - Song") != std::string::npos);
    CHECK(txt.find("ID - ID") != std::string::npos);
}
