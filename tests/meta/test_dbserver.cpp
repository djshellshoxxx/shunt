#include "TestFramework.h"
#include "FakeDbServer.h"
#include "../../meta/src/DbProtocol.h"
#include "shunt/meta/Metadata.h"
#include <condition_variable>

using namespace shunt::meta;

namespace {

constexpr uint32_t kLoopback = 0x7f000001;

struct Waiter {
    std::mutex mu;
    std::condition_variable cv;
    int done = 0;
    bool ok = false;
    TrackMetadata md;
    DbServerClient::Callback cb() {
        return [this](bool o, const TrackMetadata& m) {
            std::lock_guard<std::mutex> l(mu);
            ok = o; md = m; ++done;
            cv.notify_all();
        };
    }
    bool wait(int ms, int count = 1) {
        std::unique_lock<std::mutex> l(mu);
        return cv.wait_for(l, std::chrono::milliseconds(ms), [&] { return done >= count; });
    }
};

fake::Track sampleTrack() {
    return {4242, "Strings of Life", "Rhythim Is Rhythim", "Innovator", "Detroit Techno", "Am", "Transmat", 12150, 377};
}

int64_t msSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t).count();
}

bool runFailingMode(fake::Mode mode, int timeoutMs, int64_t* elapsed = nullptr) {
    fake::Server srv(mode);
    srv.add(sampleTrack());
    DbServerClient c;
    c.setDiscoveryPort(srv.discoveryPort());
    c.setTimeoutMs(timeoutMs);
    c.setOwnNumber(3);
    Waiter w;
    const auto t0 = std::chrono::steady_clock::now();
    c.request(kLoopback, 2, 3, 1, 4242, w.cb());
    if (!w.wait(timeoutMs * 4 + 3000)) return false;
    if (elapsed) *elapsed = msSince(t0);
    c.stop();
    return !w.ok;
}

} // namespace

TEST_CASE("dbserver codec: field round trip and UTF-16") {
    using namespace shunt::meta::db;
    Message m{77, kMsgMenuItem, {Field::u8(5), Field::u16(0xbeef), Field::u32(0xdeadbeef),
                                 Field::bytes({1, 2, 3}), Field::string("Caf\xc3\xa9 \xf0\x9f\x8e\xb5")}};
    auto bytes = encodeMessage(m);
    // magic(5) tx(5) type(3) argc(2) blob(5+12) = 32 header bytes
    REQUIRE(bytes.size() > 32);
    CHECK_EQ(int(bytes[0]), 0x11);
    CHECK_EQ(int(bytes[1]), 0x87);
    CHECK_EQ(int(bytes[15]), 0x14);
    BufferSource src(bytes.data(), bytes.size());
    Message d;
    REQUIRE(decodeMessage(src, d) == DecodeError::None);
    CHECK_EQ(src.remaining(), size_t(0));
    CHECK_EQ(d.transaction, 77u);
    CHECK_EQ(int(d.type), int(kMsgMenuItem));
    REQUIRE(d.args.size() == 5);
    CHECK_EQ(d.args[0].number, 5u);
    CHECK_EQ(d.args[1].number, 0xbeefu);
    CHECK_EQ(d.args[2].number, 0xdeadbeefu);
    CHECK(d.args[3].blob == std::vector<uint8_t>({1, 2, 3}));
    CHECK(d.args[4].text == "Caf\xc3\xa9 \xf0\x9f\x8e\xb5");
    // UTF-16 units incl. trailing NUL: "Caf\u00e9 " (5) + surrogate pair (2) + NUL (1) = 8 units.
    const auto u = utf8ToUtf16be("Caf\xc3\xa9 \xf0\x9f\x8e\xb5");
    CHECK_EQ(u.size(), size_t(16));
    CHECK(utf16beToUtf8(u.data(), u.size() / 2) == "Caf\xc3\xa9 \xf0\x9f\x8e\xb5");
}

TEST_CASE("dbserver codec: rejects garbage with bounded lengths") {
    using namespace shunt::meta::db;
    const uint8_t hugeBlob[] = {0x14, 0xff, 0xff, 0xff, 0xff};
    const uint8_t hugeStr[] = {0x26, 0x10, 0x00, 0x00, 0x00};
    const uint8_t badTag[] = {0x99, 0, 0, 0};
    const uint8_t shortNum[] = {0x11, 0x87, 0x23};
    Field f;
    { BufferSource s(hugeBlob, sizeof hugeBlob); CHECK(decodeField(s, f) == DecodeError::TooLarge); }
    { BufferSource s(hugeStr, sizeof hugeStr); CHECK(decodeField(s, f) == DecodeError::TooLarge); }
    { BufferSource s(badTag, sizeof badTag); CHECK(decodeField(s, f) == DecodeError::BadTag); }
    { BufferSource s(shortNum, sizeof shortNum); CHECK(decodeField(s, f) == DecodeError::Io); }
    std::vector<uint8_t> wrongMagic;
    encodeField(Field::u32(0x01020304), wrongMagic);
    Message m;
    { BufferSource s(wrongMagic.data(), wrongMagic.size()); CHECK(decodeMessage(s, m) == DecodeError::BadMagic); }
    auto tooMany = encodeMessage({1, 0x4000, {}});
    tooMany[14] = 13;   // argc field value
    { BufferSource s(tooMany.data(), tooMany.size()); CHECK(decodeMessage(s, m) == DecodeError::BadHeader); }
    // Unpaired surrogates become U+FFFD instead of breaking the string.
    const uint8_t lone[] = {0xd8, 0x00, 0x00, 0x41, 0, 0};
    CHECK(utf16beToUtf8(lone, 3) == "\xef\xbf\xbd" "A");
}

TEST_CASE("dbserver: discovery, greeting, metadata, render, teardown") {
    fake::Server srv;
    srv.add(sampleTrack());
    DbServerClient c;
    c.setDiscoveryPort(srv.discoveryPort());
    c.setTimeoutMs(2000);
    c.setOwnNumber(3);
    Waiter w;
    c.request(kLoopback, 2, 3, 1, 4242, w.cb());
    REQUIRE(w.wait(5000));
    REQUIRE(w.ok);
    CHECK(w.md.title == "Strings of Life");
    CHECK(w.md.artist == "Rhythim Is Rhythim");
    CHECK(w.md.album == "Innovator");
    CHECK(w.md.genre == "Detroit Techno");
    CHECK(w.md.key == "Am");
    CHECK(w.md.label == "Transmat");
    CHECK_NEAR(w.md.bpm, 121.5, 1e-9);
    CHECK_NEAR(w.md.durationS, 377, 1e-9);
    CHECK_EQ(w.md.rekordboxId, 4242u);
    CHECK(w.md.source == "dbserver");
    CHECK(!srv.badDiscoveryQuery);
    CHECK_EQ(srv.discoveryHits.load(), 1);
    CHECK_EQ(srv.greetingNumber.load(), 3u);
    CHECK_EQ(srv.dmst.load(), (3u << 24) | (1u << 16) | (3u << 8) | 1u);
    CHECK_EQ(srv.requestedId.load(), 4242u);
    CHECK_EQ(srv.renderOffset.load(), 0u);
    CHECK_EQ(srv.renderCount.load(), 8u);
    c.stop();
    for (int i = 0; i < 100 && srv.teardowns == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_EQ(srv.teardowns.load(), 1);
}

TEST_CASE("dbserver: cache by (ip, slot, id)") {
    fake::Server srv;
    srv.add(sampleTrack());
    DbServerClient c;
    c.setDiscoveryPort(srv.discoveryPort());
    c.setTimeoutMs(2000);
    c.setOwnNumber(1);
    Waiter w;
    c.request(kLoopback, 2, 3, 1, 4242, w.cb());
    c.request(kLoopback, 4, 3, 1, 4242, w.cb());   // other deck, same media and id: cached
    REQUIRE(w.wait(5000, 2));
    CHECK(w.ok);
    CHECK(w.md.title == "Strings of Life");
    CHECK_EQ(srv.sessions.load(), 1);
    Waiter w2;
    c.request(kLoopback, 2, 2, 1, 4242, w2.cb());  // different slot: new query
    REQUIRE(w2.wait(5000));
    CHECK(w2.ok);
    CHECK_EQ(srv.sessions.load(), 2);
}

TEST_CASE("dbserver: own number must be 1..6 and differ from the player") {
    fake::Server srv;
    srv.add(sampleTrack());
    DbServerClient c;
    c.setDiscoveryPort(srv.discoveryPort());
    c.setTimeoutMs(1000);
    for (uint8_t own : {uint8_t(0), uint8_t(7), uint8_t(9), uint8_t(33)}) {
        c.setOwnNumber(own);
        Waiter w;
        c.request(kLoopback, 2, 3, 1, 4242, w.cb());
        REQUIRE(w.wait(2000));
        CHECK(!w.ok);
    }
    c.setOwnNumber(2);
    Waiter w;
    c.request(kLoopback, 2, 3, 1, 4242, w.cb());
    REQUIRE(w.wait(2000));
    CHECK(!w.ok);
    CHECK_EQ(srv.discoveryHits.load(), 0);
    c.setOwnNumber(6);
    Waiter ok;
    c.request(kLoopback, 2, 3, 1, 4242, ok.cb());
    REQUIRE(ok.wait(5000));
    CHECK(ok.ok);
}

TEST_CASE("dbserver: unknown track and malformed replies fail cleanly") {
    CHECK(runFailingMode(fake::Mode::NotFound, 500));
    CHECK(runFailingMode(fake::Mode::BadMagic, 500));
    CHECK(runFailingMode(fake::Mode::WrongTransaction, 500));
    CHECK(runFailingMode(fake::Mode::HugeString, 500));
    CHECK(runFailingMode(fake::Mode::Garbage, 500));
    CHECK(runFailingMode(fake::Mode::CloseMidRender, 500));
    CHECK(runFailingMode(fake::Mode::NoFooter, 500));
    CHECK(runFailingMode(fake::Mode::DiscoveryPortZero, 500));
}

TEST_CASE("dbserver: silent player times out") {
    int64_t elapsed = 0;
    CHECK(runFailingMode(fake::Mode::Silent, 300, &elapsed));
    CHECK(elapsed >= 250);
    CHECK(elapsed < 2500);
}

TEST_CASE("dbserver: nothing listening on the discovery port") {
    uint16_t port = 0;
    fake::initNet();
    fake::Sock s = fake::listenLoopback(port);
    fake::closeSock(s);   // port now refuses connections
    DbServerClient c;
    c.setDiscoveryPort(port);
    c.setTimeoutMs(1000);
    c.setOwnNumber(3);
    Waiter w;
    c.request(kLoopback, 2, 3, 1, 4242, w.cb());
    REQUIRE(w.wait(4000));
    CHECK(!w.ok);
}

TEST_CASE("dbserver: stop() joins promptly during a hung query") {
    fake::Server srv(fake::Mode::Silent);
    srv.holdMs = 8000;
    srv.add(sampleTrack());
    DbServerClient c;
    c.setDiscoveryPort(srv.discoveryPort());
    c.setOwnNumber(3);   // default 10 s timeout
    Waiter w;
    c.request(kLoopback, 2, 3, 1, 4242, w.cb());
    c.request(kLoopback, 2, 3, 1, 4343, w.cb());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto t0 = std::chrono::steady_clock::now();
    c.stop();
    CHECK(msSince(t0) < 1500);
    CHECK(w.wait(100, 2));
    CHECK(!w.ok);
}

TEST_CASE("dbserver: client is reusable after stop()") {
    fake::Server srv;
    srv.add(sampleTrack());
    DbServerClient c;
    c.setDiscoveryPort(srv.discoveryPort());
    c.setTimeoutMs(2000);
    c.setOwnNumber(4);
    c.stop();
    Waiter w;
    c.request(kLoopback, 1, 3, 1, 4242, w.cb());
    REQUIRE(w.wait(5000));
    CHECK(w.ok);
    c.stop();
    Waiter w2;
    c.request(kLoopback, 1, 3, 1, 4242, w2.cb());   // served from cache by a fresh worker
    REQUIRE(w2.wait(5000));
    CHECK(w2.ok);
    CHECK_EQ(srv.sessions.load(), 1);
}
