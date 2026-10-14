#include "TestFramework.h"
#include "shunt/meta/Metadata.h"
#include <chrono>
#include <cstdio>
#include <string>

using namespace shunt::meta;

namespace {

std::string fixturePath() {
    const char* candidates[] = {"meta/sample_rekordbox.xml", "tests/meta/sample_rekordbox.xml",
                                "../tests/meta/sample_rekordbox.xml", "../../tests/meta/sample_rekordbox.xml"};
    for (const char* c : candidates) {
        if (std::FILE* f = std::fopen(c, "rb")) { std::fclose(f); return c; }
    }
    std::string here = __FILE__;
    const size_t slash = here.find_last_of("/\\");
    return here.substr(0, slash + 1) + "sample_rekordbox.xml";
}

std::string bigCollection(int n) {
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<DJ_PLAYLISTS Version=\"1.0.0\">\n<COLLECTION Entries=\"" +
                    std::to_string(n) + "\">\n";
    s.reserve(size_t(n) * 600);
    for (int i = 1; i <= n; ++i) {
        const std::string id = std::to_string(i);
        s += "  <TRACK TrackID=\"" + id + "\" Name=\"Track " + id + " &amp; Friends\" Artist=\"Artist " + id +
             "\" Composer=\"\" Album=\"Album\" Genre=\"Techno\" Kind=\"FLAC File\" Size=\"31234567\" TotalTime=\"" +
             std::to_string(180 + i % 300) + "\" Year=\"2020\" AverageBpm=\"" + std::to_string(120 + i % 40) +
             ".00\" BitRate=\"1411\" Tonality=\"8A\" Label=\"Label\" Location=\"file://localhost/music/" + id +
             ".flac\">\n    <TEMPO Inizio=\"0.025\" Bpm=\"128.00\" Metro=\"4/4\" Battito=\"1\"/>\n  </TRACK>\n";
    }
    s += "</COLLECTION>\n<PLAYLISTS><NODE Type=\"0\" Name=\"ROOT\" Count=\"0\"/></PLAYLISTS>\n</DJ_PLAYLISTS>\n";
    return s;
}

} // namespace

TEST_CASE("rekordbox xml: fixture fields, entities, UTF-8") {
    RekordboxXml x;
    std::string err;
    REQUIRE(x.load(fixturePath(), &err));
    CHECK_EQ(x.size(), size_t(5));
    const TrackMetadata* t = x.find(101);
    REQUIRE(t);
    CHECK(t->title == "Strings of Life");
    CHECK(t->artist == "Rhythim Is Rhythim");
    CHECK(t->album == "Innovator");
    CHECK(t->genre == "Detroit Techno");
    CHECK(t->key == "Am");
    CHECK(t->label == "Transmat");
    CHECK_NEAR(t->bpm, 121.5, 1e-9);
    CHECK_NEAR(t->durationS, 377, 1e-9);
    CHECK_EQ(t->rekordboxId, 101u);
    CHECK(t->source == "xml");

    t = x.find(102);
    REQUIRE(t);
    CHECK(t->title == "Rock & Roll <Edit> \"Live\" '93'");
    CHECK(t->artist == "Daft Punk");
    CHECK_NEAR(t->bpm, 125.0, 1e-9);
    CHECK(t->key == "4A");

    t = x.find(103);
    REQUIRE(t);
    CHECK(t->title == "Single \xe2\x80\x93 Quoted");
    CHECK(t->artist == "\xe5\x9d\x82\xe6\x9c\xac\xe9\xbe\x8d\xe4\xb8\x80");
    CHECK(t->key.empty());

    t = x.find(104);
    REQUIRE(t);
    CHECK(t->title == "Emoji \xf0\x9f\x8e\xb5 and caf\xc3\xa9");
    CHECK(t->artist == "Unknown &bogus; &#xZZ; & co");
    CHECK_NEAR(t->durationS, 300.4, 1e-9);

    t = x.find(4294967295u);
    REQUIRE(t);
    CHECK(t->title == "Max id");
    CHECK(x.find(999) == nullptr);   // only in a comment
    CHECK(x.find(555) == nullptr);   // playlist entry, not collection
    CHECK(x.find(0) == nullptr);
}

TEST_CASE("rekordbox xml: string load matches, errors reported") {
    RekordboxXml x;
    std::string err;
    CHECK(!x.load("does/not/exist.xml", &err));
    CHECK(!err.empty());

    CHECK(!x.loadFromString("<foo><COLLECTION><TRACK TrackID=\"1\"/></COLLECTION></foo>", &err));
    CHECK(err.find("DJ_PLAYLISTS") != std::string::npos);

    // Truncated inside a tag: tracks before the cut are kept, result is false.
    const std::string trunc = "<DJ_PLAYLISTS><COLLECTION><TRACK TrackID=\"1\" Name=\"A\"/><TRACK TrackID=\"2\" Na";
    CHECK(!x.loadFromString(trunc, &err));
    CHECK_EQ(x.size(), size_t(1));
    CHECK(x.find(1) != nullptr);

    // Reloading replaces the previous contents.
    REQUIRE(x.loadFromString("<DJ_PLAYLISTS><COLLECTION/><COLLECTION>"
                             "<TRACK TrackID=\"7\" Name=\"a > b\" Artist='x\"y'/></COLLECTION></DJ_PLAYLISTS>", &err));
    CHECK_EQ(x.size(), size_t(1));
    REQUIRE(x.find(7));
    CHECK(x.find(7)->title == "a > b");
    CHECK(x.find(7)->artist == "x\"y");

    // Garbage never crashes.
    std::string junk;
    for (int i = 0; i < 20000; ++i) junk += char("<>&\"'=/ !?-[]TRACKID0123"[(i * 7919) % 24]);
    x.loadFromString(junk, &err);
    x.loadFromString("<DJ_PLAYLISTS><COLLECTION><TRACK TrackID=\"5\" Name=\"&#99999999999;&#;&#x;&\"/>", &err);
    REQUIRE(x.find(5));
    CHECK(x.find(5)->title == "\xef\xbf\xbd&#;&#x;&");
}

TEST_CASE("rekordbox xml: 50k tracks streamed from disk quickly") {
    const int n = 50000;
    const std::string xml = bigCollection(n);
    const std::string path = "shunt_meta_big_test.xml";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    REQUIRE(f);
    std::fwrite(xml.data(), 1, xml.size(), f);
    std::fclose(f);
    RekordboxXml x;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = x.load(path, &err);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::remove(path.c_str());
    REQUIRE(ok);
    CHECK_EQ(x.size(), size_t(n));
    INFO("50k tracks (%llu bytes) in %lld ms\n", (unsigned long long)xml.size(), (long long)ms);
    CHECK(ms < 3000);
    const TrackMetadata* t = x.find(31337);
    REQUIRE(t);
    CHECK(t->title == "Track 31337 & Friends");
    CHECK_NEAR(t->bpm, 120 + 31337 % 40, 1e-9);
    CHECK_NEAR(t->durationS, 180 + 31337 % 300, 1e-9);
}
