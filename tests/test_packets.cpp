// ES-02 tests P-T1 to P-T4: golden decode, golden encode, length matrix, round trip.
#include "TestFramework.h"
#include "shunt/net/Packets.h"
#include <cstring>
#include <vector>

using namespace shunt::net;

namespace {

// Golden datagram built field by field from the ES-02 tables.
struct Golden {
    std::vector<uint8_t> b;
    explicit Golden(size_t len, uint8_t type, const char* name, bool announcePort) : b(len, 0) {
        std::memcpy(b.data(), kMagic, 10);
        b[0x0a] = type;
        const size_t nameAt = announcePort ? 0x0c : 0x0b;
        std::memcpy(b.data() + nameAt, name, std::strlen(name));
    }
    Golden& u8(size_t off, uint8_t v) { b[off] = v; return *this; }
    Golden& u16(size_t off, uint16_t v) { b[off] = uint8_t(v >> 8); b[off + 1] = uint8_t(v); return *this; }
    Golden& u32(size_t off, uint32_t v) { for (int i = 0; i < 4; ++i) b[off + i] = uint8_t(v >> (24 - 8 * i)); return *this; }
    Golden& bytes(size_t off, std::initializer_list<uint8_t> v) { size_t i = off; for (auto x : v) b[i++] = x; return *this; }
};

const std::array<uint8_t, 6> kMac{0x00, 0xe0, 0x36, 0x12, 0x34, 0x56};

} // namespace

// ---------------------------------------------------------------- P-T1 golden decode

TEST_CASE("P-T1 keep-alive 0x06 decodes every field") {
    Golden g(54, 0x06, "CDJ-3000", true);
    g.u8(0x20, 0x01).u8(0x21, 0x01).u16(0x22, 0x0036).u8(0x24, 3).u8(0x25, 0x01)
     .bytes(0x26, {0x00, 0xe0, 0x36, 0x12, 0x34, 0x56}).u32(0x2c, 0xa9fe0a03).u8(0x30, 5).u8(0x34, 0x01).u8(0x35, 0x64);
    auto ev = parseAnnounce(g.b.data(), g.b.size(), 1234, 0xa9fe0a03);
    REQUIRE(ev.has_value());
    CHECK(ev->type == EventType::KeepAlive);
    CHECK_EQ(ev->recvTimeNs, 1234);
    CHECK_EQ(ev->device, 3);
    CHECK(std::string(ev->keepAlive.name) == "CDJ-3000");
    CHECK(ev->keepAlive.kind == DeviceKind::CDJ);
    CHECK(ev->keepAlive.mac == kMac);
    CHECK_EQ(ev->keepAlive.ip, 0xa9fe0a03u);
    CHECK_EQ(ev->keepAlive.peerCount, 5);
    CHECK(ev->keepAlive.cdj3000Compatible);
    g.u8(0x35, 0x00).u8(0x21, 0x02);
    ev = parseAnnounce(g.b.data(), g.b.size(), 0);
    REQUIRE(ev.has_value());
    CHECK(!ev->keepAlive.cdj3000Compatible);
    CHECK(ev->keepAlive.kind == DeviceKind::Mixer);
}

TEST_CASE("P-T1 claim packets decode") {
    Golden s1(0x2c, 0x00, "CDJ-2000NXS2", true);
    s1.u8(0x24, 2).bytes(0x26, {0x00, 0xe0, 0x36, 0x12, 0x34, 0x56});
    auto ev = parseAnnounce(s1.b.data(), s1.b.size(), 0);
    REQUIRE(ev.has_value());
    CHECK(ev->type == EventType::Claim);
    CHECK_EQ(ev->claim.packetType, 0x00);
    CHECK_EQ(ev->claim.counter, 2);
    CHECK(ev->claim.mac == kMac);

    Golden s2(0x32, 0x02, "CDJ-2000NXS2", true);
    s2.u32(0x24, 0xc0a80105).bytes(0x28, {0x00, 0xe0, 0x36, 0x12, 0x34, 0x56}).u8(0x2e, 7).u8(0x2f, 3).u8(0x31, 0x01);
    ev = parseAnnounce(s2.b.data(), s2.b.size(), 0);
    REQUIRE(ev.has_value());
    CHECK_EQ(ev->claim.packetType, 0x02);
    CHECK_EQ(ev->claim.ip, 0xc0a80105u);
    CHECK(ev->claim.mac == kMac);
    CHECK_EQ(ev->claim.number, 7);
    CHECK_EQ(ev->claim.counter, 3);
    CHECK_EQ(ev->claim.autoFlag, 0x01);

    Golden s3(0x2a, 0x04, "CDJ-2000NXS2", true);
    s3.u8(0x24, 7).u8(0x25, 1);
    ev = parseAnnounce(s3.b.data(), s3.b.size(), 0);
    REQUIRE(ev.has_value());
    CHECK_EQ(ev->claim.packetType, 0x04);
    CHECK_EQ(ev->claim.number, 7);
    CHECK_EQ(ev->claim.counter, 1);

    Golden d(0x29, 0x08, "CDJ-2000NXS2", true);
    d.u8(0x24, 7);
    ev = parseAnnounce(d.b.data(), d.b.size(), 0);
    REQUIRE(ev.has_value());
    CHECK_EQ(ev->claim.packetType, 0x08);
    CHECK_EQ(ev->claim.number, 7);

    Golden h(0x25, 0x0a, "CDJ-2000NXS2", true);
    ev = parseAnnounce(h.b.data(), h.b.size(), 0);
    REQUIRE(ev.has_value());
    CHECK_EQ(ev->claim.packetType, 0x0a);
    CHECK(std::string(ev->claim.name) == "CDJ-2000NXS2");

    Golden m(0x26, 0x05, "DJM-900NXS2", true);
    m.u8(0x24, 2);
    ev = parseAnnounce(m.b.data(), m.b.size(), 0);
    REQUIRE(ev.has_value());
    CHECK_EQ(ev->claim.packetType, 0x05);
}

TEST_CASE("P-T1 beat 0x28 decodes every field") {
    Golden g(0x60, 0x28, "CDJ-2000NXS2", false);
    g.u8(0x21, 2).u32(0x24, 469).u32(0x28, 938).u32(0x2c, 1406).u32(0x30, 1875).u32(0x34, 3281)
     .u32(0x38, 0xffffffff).u32(0x54, 0x00100000 + 0x00010000).u16(0x5a, 12800).u8(0x5c, 3);
    auto ev = parseBeatPort(g.b.data(), g.b.size(), 99);
    REQUIRE(ev.has_value());
    CHECK(ev->type == EventType::Beat);
    CHECK_EQ(ev->device, 2);
    CHECK_EQ(ev->beat.nextBeatMs, 469u);
    CHECK_EQ(ev->beat.secondBeatMs, 938u);
    CHECK_EQ(ev->beat.nextBarMs, 1406u);
    CHECK_EQ(ev->beat.fourthBeatMs, 1875u);
    CHECK_EQ(ev->beat.secondBarMs, 3281u);
    CHECK_EQ(ev->beat.eighthBeatMs, 0xffffffffu);
    CHECK_NEAR(ev->beat.pitch, 1.0625, 1e-9);
    CHECK_EQ(ev->beat.bpm100, 12800);
    CHECK_NEAR(ev->beat.effectiveBpm, 136.0, 1e-9);
    CHECK_EQ(ev->beat.beatInBar, 3);
    CHECK(!ev->beat.isMaster);
    // mixer beat: beatInBar ignored; unknown bpm
    g.u8(0x21, 0x21).u16(0x5a, 0xffff);
    ev = parseBeatPort(g.b.data(), g.b.size(), 0);
    REQUIRE(ev.has_value());
    CHECK_EQ(ev->beat.beatInBar, 0);
    CHECK_NEAR(ev->beat.effectiveBpm, 0.0, 1e-9);
    // too short
    CHECK(!parseBeatPort(g.b.data(), 0x5f, 0).has_value());
}

TEST_CASE("P-T1 precise position, on-air and control decode") {
    Golden p(0x3c, 0x0b, "CDJ-3000", false);
    p.u8(0x21, 1).u32(0x24, 300).u32(0x28, uint32_t(int32_t(-1500))).u32(0x2c, uint32_t(int32_t(-128))).u32(0x38, 1280);
    auto ev = parseBeatPort(p.b.data(), p.b.size(), 0);
    REQUIRE(ev.has_value());
    CHECK(ev->type == EventType::PrecisePosition);
    CHECK_EQ(ev->precise.trackLengthS, 300u);
    CHECK_EQ(ev->precise.playheadMs, -1500);
    CHECK_NEAR(ev->precise.pitchPercent, -2.0, 1e-9);
    CHECK_EQ(ev->precise.bpm10, 1280u);

    Golden o4(0x2d, 0x03, "DJM-900NXS2", false);
    o4.u8(0x21, 0x21).bytes(0x24, {1, 0, 1, 0});
    ev = parseBeatPort(o4.b.data(), o4.b.size(), 0);
    REQUIRE(ev.has_value());
    CHECK(ev->type == EventType::OnAir);
    CHECK_EQ(ev->onAir.channelCount, 4);
    CHECK_EQ(ev->onAir.channels[0], 1); CHECK_EQ(ev->onAir.channels[1], 0);
    CHECK_EQ(ev->onAir.channels[2], 1); CHECK_EQ(ev->onAir.channels[3], 0);

    Golden o6(0x35, 0x03, "DJM-V10", false);
    o6.u8(0x20, 0x03).u8(0x21, 0x21).bytes(0x24, {0, 1, 0, 1}).bytes(0x2d, {1, 1});
    ev = parseBeatPort(o6.b.data(), o6.b.size(), 0);
    REQUIRE(ev.has_value());
    CHECK_EQ(ev->onAir.channelCount, 6);
    CHECK_EQ(ev->onAir.channels[4], 1); CHECK_EQ(ev->onAir.channels[5], 1);

    for (uint8_t t : {0x02, 0x26, 0x27, 0x2a}) {
        Golden c(0x30, t, "CDJ-2000NXS2", false);
        c.u8(0x21, 4).u8(0x24, 0x7f);
        ev = parseBeatPort(c.b.data(), c.b.size(), 0);
        REQUIRE(ev.has_value());
        CHECK(ev->type == EventType::Control);
        CHECK_EQ(ev->control.packetType, t);
        CHECK_EQ(ev->control.payload[0], 0x7f);
    }
}

TEST_CASE("P-T1 CDJ status 0x0a decodes every field") {
    Golden g(0x200, 0x0a, "CDJ-3000", false);
    g.u8(0x21, 2).u8(0x27, 0x06).u8(0x28, 2).u8(0x29, 3).u8(0x2a, 1).u32(0x2c, 4711).u16(0x32, 12)
     .u8(0x7b, 3).bytes(0x7c, {'3', '.', '1', '0'}).u32(0x84, 42).u8(0x89, 0x40 | 0x20 | 0x10 | 0x08 | 0x02)
     .u32(0x8c, 0x00100000).u16(0x92, 12800).u8(0x9d, 1).u8(0x9e, 2).u8(0x9f, 0xff).u32(0xa0, 257)
     .u16(0xa4, 0x01ff).u8(0xa6, 2).u32(0xc0, 0x00100000).u8(0xcc, 0x1f).bytes(0x15c, {0x08, 0x01, 0x02});
    auto ev = parseStatusPort(g.b.data(), g.b.size(), 5);
    REQUIRE(ev.has_value());
    CHECK(ev->type == EventType::PlayerStatus);
    const auto& s = ev->status;
    CHECK_EQ(ev->device, 2);
    CHECK_EQ(s.activity, 0x06);
    CHECK_EQ(s.sourcePlayer, 2);
    CHECK_EQ(s.slot, 3);
    CHECK_EQ(s.trackType, 1);
    CHECK_EQ(s.rekordboxId, 4711u);
    CHECK_EQ(s.trackNumber, 12);
    CHECK_EQ(s.playState, 3);
    CHECK(std::string(s.firmware) == "3.10");
    CHECK(s.playing); CHECK(s.master); CHECK(s.synced); CHECK(s.onAir); CHECK(s.bpmSync);
    CHECK_NEAR(s.pitch1, 1.0, 1e-9);
    CHECK_EQ(s.bpm100, 12800);
    CHECK_EQ(s.p3, 1);
    CHECK_EQ(s.masterMeaning, 2);
    CHECK_EQ(s.handoffTo, 0xff);
    CHECK_EQ(s.beatNumber, 257u);
    CHECK_EQ(s.cueCountdown, 0x01ff);
    CHECK_EQ(s.beatInBar, 2);
    CHECK_EQ(s.nx, 0x1f);
    CHECK(s.hasFByte);
    CHECK(s.hasKey);
    CHECK_EQ(s.key[0], 0x08);
    CHECK_EQ(s.packetLength, 0x200);
}

TEST_CASE("P-T1 pre-nexus status derives play and master without F byte") {
    Golden g(0xd0, 0x0a, "CDJ-2000", false);
    g.u8(0x21, 1).u8(0x7b, 4).u8(0x9e, 1).u8(0x89, 0x00);
    auto ev = parseStatusPort(g.b.data(), g.b.size(), 0);
    REQUIRE(ev.has_value());
    CHECK(!ev->status.hasFByte);
    CHECK(ev->status.playing);
    CHECK(ev->status.master);
    g.u8(0x7b, 5);
    ev = parseStatusPort(g.b.data(), g.b.size(), 0);
    CHECK(!ev->status.playing);
}

TEST_CASE("P-T1 mixer status 0x29 and rekordbox subtype decode") {
    Golden g(0x38, 0x29, "DJM-900NXS2", false);
    g.u8(0x21, 0x21).u8(0x27, 0xf0).u16(0x2e, 12850).u8(0x36, 0x02).u8(0x37, 3);
    auto ev = parseStatusPort(g.b.data(), g.b.size(), 0);
    REQUIRE(ev.has_value());
    CHECK(ev->type == EventType::MixerStatus);
    CHECK_EQ(ev->device, 0x21);
    CHECK(ev->mixer.master);
    CHECK_EQ(ev->mixer.flags, 0xf0);
    CHECK_EQ(ev->mixer.bpm100, 12850);
    CHECK_EQ(ev->mixer.handoffTo, 0x02);
    CHECK_EQ(ev->mixer.beatInBar, 3);
    CHECK(!ev->mixer.fromRekordbox);
    g.u8(0x27, 0xd0).u8(0x20, 0x01).u16(0x22, 0x38);
    ev = parseStatusPort(g.b.data(), g.b.size(), 0);
    REQUIRE(ev.has_value());
    CHECK(!ev->mixer.master);
    CHECK(ev->mixer.fromRekordbox);
}

TEST_CASE("P-T1 bad magic and short packets are rejected") {
    Golden g(54, 0x06, "CDJ-3000", true);
    g.b[0] = 0x00;
    CHECK(!parseAnnounce(g.b.data(), g.b.size(), 0).has_value());
    Golden k(54, 0x06, "CDJ-3000", true);
    CHECK(!parseAnnounce(k.b.data(), 0x23, 0).has_value());
    CHECK(!parseAnnounce(k.b.data(), 53, 0).has_value());
    CHECK(!parseAnnounce(nullptr, 0, 0).has_value());
    Golden unknown(0x40, 0x77, "X", true);
    CHECK(!parseAnnounce(unknown.b.data(), unknown.b.size(), 0).has_value());
    CHECK(!parseBeatPort(unknown.b.data(), unknown.b.size(), 0).has_value());
    CHECK(!parseStatusPort(unknown.b.data(), unknown.b.size(), 0).has_value());
}

// ---------------------------------------------------------------- P-T2 golden encode

TEST_CASE("P-T2 keep-alive builder: 54 bytes, documented layout, 0x35 == 0x64") {
    uint8_t out[128];
    KeepAliveParams p;
    p.number = 7; p.mac = kMac; p.ip = 0xc0a80105; p.peerCount = 6;
    const size_t n = buildKeepAlive(out, p);
    CHECK_EQ(n, kKeepAliveLen);
    Golden g(54, 0x06, "Shunt", true);
    g.u8(0x20, 0x01).u8(0x21, 0x01).u16(0x22, 0x0036).u8(0x24, 7).u8(0x25, 0x01)
     .bytes(0x26, {0x00, 0xe0, 0x36, 0x12, 0x34, 0x56}).u32(0x2c, 0xc0a80105).u8(0x30, 6).u8(0x34, 0x01).u8(0x35, 0x64);
    CHECK(std::memcmp(out, g.b.data(), 54) == 0);
    CHECK_EQ(out[0x35], 0x64);
}

TEST_CASE("P-T2 claim builders produce documented lengths") {
    uint8_t out[128];
    CHECK_EQ(buildHello(out, "Shunt", false), kHelloLen);
    CHECK_EQ(buildHello(out, "Shunt", true), kHelloLen3000);
    CHECK_EQ(out[0x0a], 0x0a);
    CHECK_EQ(buildClaimStage1(out, "Shunt", 1, kMac), kClaim1Len);
    CHECK_EQ(out[0x0a], 0x00); CHECK_EQ(out[0x24], 1);
    CHECK_EQ(buildClaimStage2(out, "Shunt", 0xc0a80105, kMac, 7, 2, 0x01), kClaim2Len);
    CHECK_EQ(out[0x0a], 0x02); CHECK_EQ(out[0x2e], 7); CHECK_EQ(out[0x2f], 2); CHECK_EQ(out[0x31], 0x01);
    CHECK_EQ(buildClaimStage3(out, "Shunt", 7, 3), kClaim3Len);
    CHECK_EQ(out[0x0a], 0x04); CHECK_EQ(out[0x24], 7); CHECK_EQ(out[0x25], 3);
    CHECK_EQ(buildDefend(out, "Shunt", 7), kDefendLen);
    CHECK_EQ(out[0x0a], 0x08); CHECK_EQ(out[0x24], 7);
    CHECK(std::memcmp(out, kMagic, 10) == 0);
    CHECK(std::string(reinterpret_cast<char*>(out + 0x0c)) == "Shunt");
}

// ---------------------------------------------------------------- P-T3 length matrix

TEST_CASE("P-T3 status length matrix") {
    for (size_t len : {0xd0, 0xd4, 0x11b, 0x11c, 0x124, 0x200, 0x300}) {
        Golden g(len, 0x0a, "CDJ", false);
        g.u8(0x21, 1).u16(0x92, 12000);
        auto ev = parseStatusPort(g.b.data(), g.b.size(), 0);
        REQUIRE(ev.has_value());
        CHECK_EQ(ev->status.bpm100, 12000);
        CHECK_EQ(ev->status.packetLength, uint16_t(len));
        CHECK(ev->status.hasKey == (len >= 0x15f));
    }
    for (size_t len : {0x24, 0x60, 0xcb}) {
        Golden g(len, 0x0a, "CDJ", false);
        CHECK(!parseStatusPort(g.b.data(), g.b.size(), 0).has_value());
    }
    Golden exact(0xcc, 0x0a, "CDJ", false);
    CHECK(parseStatusPort(exact.b.data(), exact.b.size(), 0).has_value());
    Golden mixerShort(0x37, 0x29, "DJM", false);
    CHECK(!parseStatusPort(mixerShort.b.data(), mixerShort.b.size(), 0).has_value());
}

// ---------------------------------------------------------------- P-T4 round trip

TEST_CASE("P-T4 round trip keep-alive and claims") {
    uint8_t out[128];
    KeepAliveParams p;
    p.number = 9; p.mac = kMac; p.ip = 0xa9fe0101; p.peerCount = 3; p.cdj3000Compatible = true;
    size_t n = buildKeepAlive(out, p);
    auto ev = parseAnnounce(out, n, 0);
    REQUIRE(ev.has_value());
    CHECK_EQ(ev->keepAlive.number, 9);
    CHECK(ev->keepAlive.mac == kMac);
    CHECK_EQ(ev->keepAlive.ip, 0xa9fe0101u);
    CHECK_EQ(ev->keepAlive.peerCount, 3);
    CHECK(ev->keepAlive.cdj3000Compatible);
    CHECK(std::string(ev->keepAlive.name) == "Shunt");

    n = buildClaimStage2(out, "Shunt", 0xa9fe0101, kMac, 8, 2, 0x02);
    ev = parseAnnounce(out, n, 0);
    REQUIRE(ev.has_value());
    CHECK_EQ(ev->claim.ip, 0xa9fe0101u); CHECK(ev->claim.mac == kMac);
    CHECK_EQ(ev->claim.number, 8); CHECK_EQ(ev->claim.counter, 2); CHECK_EQ(ev->claim.autoFlag, 0x02);
    n = buildClaimStage1(out, "Shunt", 3, kMac);
    ev = parseAnnounce(out, n, 0);
    REQUIRE(ev.has_value());
    CHECK_EQ(ev->claim.counter, 3); CHECK(ev->claim.mac == kMac);
    n = buildClaimStage3(out, "Shunt", 8, 1);
    ev = parseAnnounce(out, n, 0);
    REQUIRE(ev.has_value());
    CHECK_EQ(ev->claim.number, 8); CHECK_EQ(ev->claim.counter, 1);
    n = buildDefend(out, "Shunt", 8);
    ev = parseAnnounce(out, n, 0);
    REQUIRE(ev.has_value());
    CHECK_EQ(ev->claim.number, 8);
}

TEST_CASE("P-T4 round trip beat, status, mixer status, on-air, precise") {
    uint8_t out[1024];
    BeatParams b;
    b.number = 3; b.nextBeatMs = 100; b.nextBarMs = 1000; b.eighthBeatMs = 0xffffffff; b.pitch = 1.04; b.bpm100 = 12345; b.beatInBar = 4;
    size_t n = buildBeat(out, b);
    CHECK_EQ(n, kBeatLen);
    auto ev = parseBeatPort(out, n, 0);
    REQUIRE(ev.has_value());
    CHECK_EQ(ev->device, 3);
    CHECK_EQ(ev->beat.nextBeatMs, 100u); CHECK_EQ(ev->beat.nextBarMs, 1000u); CHECK_EQ(ev->beat.eighthBeatMs, 0xffffffffu);
    CHECK_NEAR(ev->beat.pitch, 1.04, 1e-6);
    CHECK_EQ(ev->beat.bpm100, 12345); CHECK_EQ(ev->beat.beatInBar, 4);
    CHECK_NEAR(ev->beat.effectiveBpm, 123.45 * 1.04, 1e-4);

    CdjStatusParams s;
    s.number = 2; s.length = 0x11c; s.slot = 3; s.trackType = 1; s.rekordboxId = 99; s.trackNumber = 7; s.playState = 3;
    s.flags = kFlagPlay | kFlagMaster; s.pitch1 = 0.98; s.bpm100 = 12800; s.masterMeaning = 2; s.handoffTo = 4;
    s.beatNumber = 123; s.beatInBar = 2; s.nx = 0x0f;
    n = buildCdjStatus(out, s);
    CHECK_EQ(n, size_t(0x11c));
    ev = parseStatusPort(out, n, 0);
    REQUIRE(ev.has_value());
    CHECK_EQ(ev->device, 2);
    CHECK_EQ(ev->status.slot, 3); CHECK_EQ(ev->status.rekordboxId, 99u); CHECK_EQ(ev->status.trackNumber, 7);
    CHECK(ev->status.playing); CHECK(ev->status.master); CHECK(!ev->status.synced);
    CHECK_NEAR(ev->status.pitch1, 0.98, 1e-6);
    CHECK_EQ(ev->status.bpm100, 12800); CHECK_EQ(ev->status.masterMeaning, 2); CHECK_EQ(ev->status.handoffTo, 4);
    CHECK_EQ(ev->status.beatNumber, 123u); CHECK_EQ(ev->status.beatInBar, 2); CHECK_EQ(ev->status.nx, 0x0f);
    CHECK(std::string(ev->status.firmware) == "1.00");

    MixerStatusParams m;
    m.flags = 0xf0; m.bpm100 = 13000; m.handoffTo = 1; m.beatInBar = 2;
    n = buildMixerStatus(out, m);
    CHECK_EQ(n, kMixerStatusLen);
    ev = parseStatusPort(out, n, 0);
    REQUIRE(ev.has_value());
    CHECK(ev->mixer.master); CHECK_EQ(ev->mixer.bpm100, 13000); CHECK_EQ(ev->mixer.handoffTo, 1);

    const uint8_t ch[6] = {1, 0, 0, 1, 1, 0};
    n = buildOnAir(out, ch, true);
    ev = parseBeatPort(out, n, 0);
    REQUIRE(ev.has_value());
    CHECK_EQ(ev->onAir.channelCount, 6);
    CHECK_EQ(ev->onAir.channels[3], 1); CHECK_EQ(ev->onAir.channels[4], 1); CHECK_EQ(ev->onAir.channels[5], 0);
    n = buildOnAir(out, ch, false);
    ev = parseBeatPort(out, n, 0);
    REQUIRE(ev.has_value());
    CHECK_EQ(ev->onAir.channelCount, 4);

    n = buildPrecisePosition(out, "CDJ-3000", 1, 240, 12345, 1.5, 1280);
    ev = parseBeatPort(out, n, 0);
    REQUIRE(ev.has_value());
    CHECK_EQ(ev->precise.trackLengthS, 240u); CHECK_EQ(ev->precise.playheadMs, 12345);
    CHECK_NEAR(ev->precise.pitchPercent, 1.5, 1e-9); CHECK_EQ(ev->precise.bpm10, 1280u);
}

TEST_CASE("pitch encode/decode") {
    CHECK_NEAR(decodePitch(0x100000), 1.0, 1e-12);
    CHECK_EQ(encodePitch(1.0), 0x100000u);
    CHECK_NEAR(decodePitch(encodePitch(0.9375)), 0.9375, 1e-6);
    CHECK_NEAR(decodePitch(0xff100000), 1.0, 1e-12);   // top byte ignored
}
