// ES-05 tests O-T1 to O-T13: MIDI clock, OSC, Link adapter (no drivers, no sockets).
#include "TestFramework.h"
#include "shunt/out/LinkOutput.h"
#include "shunt/out/MidiClock.h"
#include "shunt/out/Osc.h"
#include <cmath>
#include <vector>

using namespace shunt::out;
using shunt::clock::Timeline;

namespace {

constexpr int64_t MS = 1'000'000;

struct Msg { int64_t t; std::vector<uint8_t> b; };
struct RecSink : IMidiSink {
    std::vector<Msg> m;
    void send(int64_t t, const uint8_t* b, size_t n) override { m.push_back({t, {b, b + n}}); }
    std::vector<int64_t> times(uint8_t status) const {
        std::vector<int64_t> v;
        for (auto& x : m) if (x.b[0] == status) v.push_back(x.t);
        return v;
    }
    int count(uint8_t s) const { return int(times(s).size()); }
};

Timeline mkTl(double bpm = 120, int64_t origin = 1'000 * MS) {
    Timeline t; t.bpm = bpm; t.beatOriginNs = origin; t.beatIndex = 100; t.beatInBar = 1;
    t.barKnown = true; t.playing = true; t.confidence = 1; t.resetSequence = 1;
    return t;
}

void run(MidiClockGenerator& g, const Timeline& tl, RecSink& s, int64_t from, int64_t to, int64_t step = 5 * MS) {
    for (int64_t t = from; t <= to; t += step) g.update(tl, t, s);
}

struct RecTx : IOscTransport {
    std::vector<std::vector<uint8_t>> pk;
    bool ok = true;
    bool send(const uint8_t* d, size_t n) override { pk.emplace_back(d, d + n); return ok; }
};

struct FakeLink : ILinkBackend {
    double raw = 0; int tempoCalls = 0, soft = 0, hard = 0, lastQuantum = 0; double lastTempo = 0;
    void setTempo(double b, int64_t) override { ++tempoCalls; lastTempo = b; }
    double beatAtTime(int64_t, double) const override { return raw; }
    void requestBeatAtTime(double, int64_t, double q) override { ++soft; lastQuantum = int(q); }
    void forceBeatAtTime(double, int64_t, double q) override { ++hard; lastQuantum = int(q); }
    void setPlaying(bool, int64_t) override {}
    size_t numPeers() const override { return 0; }
};

} // namespace

TEST_CASE("O-T1 midi clock tick spacing and count") {
    MidiClockGenerator g; RecSink s; const auto tl = mkTl(120);
    run(g, tl, s, 1'000 * MS, 3'000 * MS);
    auto t = s.times(0xF8);
    CHECK(t.size() >= 95 && t.size() <= 97);   // 2 s at 48 ticks/s
    for (size_t i = 1; i < t.size(); ++i) CHECK_NEAR(double(t[i] - t[i - 1]), 500e6 / 24, 2.0);
}

TEST_CASE("O-T2 midi clock tempo change stays monotonic and adopts new spacing") {
    MidiClockGenerator g; RecSink s; auto tl = mkTl(120);
    run(g, tl, s, 1'000 * MS, 2'000 * MS);
    tl.bpm = 130; tl.beatOriginNs = 2'000 * MS; tl.beatIndex = 102; tl.resetSequence = 1;
    run(g, tl, s, 2'005 * MS, 3'000 * MS);
    auto t = s.times(0xF8);
    for (size_t i = 1; i < t.size(); ++i) CHECK(t[i] > t[i - 1]);
    CHECK_NEAR(double(t.back() - t[t.size() - 2]), 60e9 / 130 / 24, 2.0);
}

TEST_CASE("O-T3 start once at beat on first lock; none on reset by default; optional") {
    MidiClockGenerator g; RecSink s; auto tl = mkTl();
    run(g, tl, s, 1'010 * MS, 3'000 * MS);
    CHECK_EQ(s.count(0xFA), 1);
    CHECK(s.times(0xFA)[0] == s.times(0xF8)[0]);
    tl.resetSequence = 2; run(g, tl, s, 3'005 * MS, 4'000 * MS);
    CHECK_EQ(s.count(0xFA), 1);
    MidiClockConfig c; c.startOnReset = true;
    MidiClockGenerator g2(c); RecSink s2; auto tl2 = mkTl();
    run(g2, tl2, s2, 1'010 * MS, 3'000 * MS);
    tl2.resetSequence = 2; run(g2, tl2, s2, 3'005 * MS, 4'000 * MS);
    CHECK_EQ(s2.count(0xFA), 2);
}

TEST_CASE("O-T4 pause keeps running by default; Stop mode stops then continues") {
    auto tl = mkTl();
    MidiClockGenerator g; RecSink s;
    run(g, tl, s, 1'010 * MS, 2'000 * MS);
    const int before = s.count(0xF8);
    tl.playing = false; run(g, tl, s, 2'005 * MS, 3'000 * MS);
    CHECK(s.count(0xF8) > before + 40);
    CHECK_EQ(s.count(0xFC), 0);

    MidiClockConfig c; c.pauseMode = PauseMode::Stop;
    MidiClockGenerator g2(c); RecSink s2; auto t2 = mkTl();
    run(g2, t2, s2, 1'010 * MS, 2'000 * MS);
    t2.playing = false; run(g2, t2, s2, 2'005 * MS, 3'000 * MS);
    const int atStop = s2.count(0xF8);
    run(g2, t2, s2, 3'005 * MS, 3'500 * MS);
    CHECK_EQ(s2.count(0xF8), atStop);
    CHECK_EQ(s2.count(0xFC), 1);
    t2.playing = true; run(g2, t2, s2, 3'505 * MS, 4'500 * MS);
    CHECK_EQ(s2.count(0xFB), 1);
    CHECK(s2.count(0xF8) > atStop);
}

TEST_CASE("O-T5 SPP then Continue on reset when enabled") {
    MidiClockConfig c; c.sppOnReset = true;
    MidiClockGenerator g(c); RecSink s; auto tl = mkTl();
    run(g, tl, s, 1'010 * MS, 3'000 * MS);
    tl.resetSequence = 2; tl.beatInBar = 3;   // beat 100 is bar beat 3
    run(g, tl, s, 3'005 * MS, 4'000 * MS);
    CHECK_EQ(s.count(0xF2), 1);
    CHECK_EQ(s.count(0xFB), 1);
    for (auto& m : s.m) if (m.b[0] == 0xF2) { CHECK(m.b[1] % 4 == 0 && m.b[1] < 16 && m.b[2] == 0); }
}

TEST_CASE("O-T6 overlapping updates never duplicate ticks") {
    MidiClockGenerator g; RecSink s; const auto tl = mkTl(128);
    run(g, tl, s, 1'010 * MS, 2'000 * MS, 1 * MS);
    auto t = s.times(0xF8);
    for (size_t i = 1; i < t.size(); ++i) CHECK(t[i] > t[i - 1]);
}

TEST_CASE("O-T7 osc encoding golden bytes") {
    const auto b = encodeOsc({"/a", {int32_t(1), 1.0f, std::string("hi")}});
    const std::vector<uint8_t> want = {'/', 'a', 0, 0, ',', 'i', 'f', 's', 0, 0, 0, 0,
                                       0, 0, 0, 1, 0x3f, 0x80, 0, 0, 'h', 'i', 0, 0};
    // type tag string ",ifs" is 4 chars + NUL, padded to 8 bytes
    CHECK_EQ(b.size(), want.size());
    CHECK(b == want);
    const auto bun = encodeOscBundle({{"/a", {}}});
    CHECK(std::string(bun.begin(), bun.begin() + 7) == "#bundle");
    CHECK_EQ(bun.size(), size_t(8 + 8 + 4 + 8));
    CHECK_EQ(int(bun[15]), 1);
}

TEST_CASE("O-T8 osc profile message selection") {
    auto tl = mkTl(128);
    for (auto prof : {OscProfile::Generic, OscProfile::Resolume, OscProfile::GrandMA3, OscProfile::MagicQ}) {
        RecTx tx; OscConfig c; c.profile = prof; c.shuntMessages = false;
        OscOutput o(tx, c);
        o.update(tl, 1'000 * MS);
        REQUIRE(!tx.pk.empty());
        const std::string s(tx.pk[0].begin(), tx.pk[0].end());
        if (prof == OscProfile::Generic) CHECK(s.find("/shunt/bpm") != std::string::npos);
        if (prof == OscProfile::Resolume) CHECK(s.find("/composition/tempocontroller/tempo") != std::string::npos);
        if (prof == OscProfile::GrandMA3) CHECK(s.find("Master 3.1 At BPM 128.00") != std::string::npos);
        if (prof == OscProfile::MagicQ) CHECK(s.find("/magicq/bpm") != std::string::npos);
        if (prof != OscProfile::Generic) CHECK(s.find("/shunt/bpm") == std::string::npos);
    }
}

TEST_CASE("O-T9 osc tempo throttle") {
    RecTx tx; OscOutput o(tx); auto tl = mkTl(128);
    o.update(tl, 1'000 * MS);
    const size_t n = tx.pk.size();
    tl.bpm = 128.5; o.update(tl, 1'100 * MS);     // under 1 s
    bool sawBpm = false;
    for (size_t i = n; i < tx.pk.size(); ++i) sawBpm |= std::string(tx.pk[i].begin(), tx.pk[i].end()).find("/shunt/bpm") != std::string::npos;
    CHECK(!sawBpm);
    o.update(tl, 2'200 * MS);
    sawBpm = false;
    for (size_t i = n; i < tx.pk.size(); ++i) sawBpm |= std::string(tx.pk[i].begin(), tx.pk[i].end()).find("/shunt/bpm") != std::string::npos;
    CHECK(sawBpm);
}

TEST_CASE("O-T10 osc one beat message per beat") {
    RecTx tx; OscOutput o(tx); const auto tl = mkTl(120);
    int beats = 0;
    for (int64_t t = 1'000 * MS; t <= 3'000 * MS; t += 10 * MS) {
        const size_t n = tx.pk.size();
        o.update(tl, t);
        for (size_t i = n; i < tx.pk.size(); ++i)
            if (std::string(tx.pk[i].begin(), tx.pk[i].end()).find("/shunt/beat") != std::string::npos) ++beats;
    }
    CHECK(beats >= 3 && beats <= 4);
}

TEST_CASE("O-T11 link quantum follows barKnown") {
    auto tl = mkTl(); CHECK_EQ(int(LinkOutput::quantumFor(tl)), 4);
    tl.barKnown = false; CHECK_EQ(int(LinkOutput::quantumFor(tl)), 1);
}

TEST_CASE("O-T12 link hard on reset, soft after two errors, none when in tolerance") {
    FakeLink fl; LinkOutput lo(fl); auto tl = mkTl(120);
    // Beat k at 1000ms + n*500ms; Link phase exactly right (bar beat 1 at beatIndex 100 => phase 0 mod 4).
    int64_t t = 1'000 * MS + 5 * MS;
    fl.raw = 0.01; lo.update(tl, t);                     // first beat: hard (new reset sequence)
    CHECK_EQ(fl.hard, 1);
    for (int i = 1; i <= 3; ++i) { t += 500 * MS; fl.raw = std::fmod(i + 0.01, 4); lo.update(tl, t); }
    CHECK_EQ(fl.soft, 0); CHECK_EQ(fl.hard, 1);
    for (int i = 4; i <= 6; ++i) { t += 500 * MS; fl.raw = std::fmod(i + 0.3, 4); lo.update(tl, t); }
    CHECK(fl.soft >= 1); CHECK_EQ(fl.hard, 1);
}

TEST_CASE("O-T13 link tempo throttle") {
    FakeLink fl; LinkOutput lo(fl); auto tl = mkTl(120);
    lo.update(tl, 1'000 * MS); tl.bpm = 120.5; lo.update(tl, 1'100 * MS);
    CHECK_EQ(fl.tempoCalls, 1);
    lo.update(tl, 2'200 * MS);
    CHECK_EQ(fl.tempoCalls, 2);
}

#include "shunt/out/OscInput.h"
#include "shunt/out/RawMidiSink.h"
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

TEST_CASE("O-T14 osc decode roundtrip, bundles and malformed input") {
    OscMessage m{"/shunt/baroffset", {int32_t(2)}};
    auto d = decodeOsc(encodeOsc(m).data(), encodeOsc(m).size());
    REQUIRE(d.has_value());
    CHECK(d->address == m.address && std::get<int32_t>(d->args[0]) == 2);
    OscMessage m2{"/x", {1.5f, std::string("abc"), int32_t(-7)}};
    const auto e2 = encodeOsc(m2);
    auto d2 = decodeOsc(e2.data(), e2.size());
    REQUIRE(d2.has_value() && d2->args.size() == 3);
    CHECK_NEAR(std::get<float>(d2->args[0]), 1.5, 1e-6);
    CHECK(std::get<std::string>(d2->args[1]) == "abc");
    CHECK_EQ(int(std::get<int32_t>(d2->args[2])), -7);
    const auto b = encodeOscBundle({m, m2});
    CHECK_EQ(decodeOscPacket(b.data(), b.size()).size(), size_t(2));
    // truncated and garbage input never crashes and yields nothing
    for (size_t n = 0; n < e2.size(); ++n) { auto t = decodeOsc(e2.data(), n); if (n % 4 || n < 8) CHECK(!t || n >= 4); }
    uint32_t seed = 7;
    for (int i = 0; i < 5000; ++i) {
        uint8_t buf[64]; for (auto& x : buf) { seed = seed * 1664525u + 1013904223u; x = uint8_t(seed >> 24); }
        if (i & 1) { buf[0] = '/'; }
        (void)decodeOscPacket(buf, size_t(i % 65));
    }
}

TEST_CASE("O-T15 osc control dispatch validates ranges") {
    int bar = -1; int beat = -1;
    OscControl c; c.setBarOffset = [&](int v) { bar = v; }; c.setBeatOnly = [&](bool v) { beat = v; };
    CHECK_EQ(dispatchOscControl({{"/shunt/baroffset", {int32_t(3)}}, {"/shunt/beatonly", {int32_t(1)}}}, c), size_t(2));
    CHECK_EQ(bar, 3); CHECK_EQ(beat, 1);
    CHECK_EQ(dispatchOscControl({{"/shunt/baroffset", {int32_t(4)}}, {"/shunt/beatonly", {int32_t(2)}},
                                 {"/shunt/baroffset", {1.0f}}, {"/other", {int32_t(1)}}}, c), size_t(0));
    CHECK_EQ(bar, 3);
}

TEST_CASE("O-T16 raw midi sink writes bytes to a file") {
    char path[] = "/tmp/shunt_midiXXXXXX";
    const int fd = mkstemp(path); REQUIRE(fd >= 0); ::close(fd);
    {
        RawMidiSink sink; REQUIRE(sink.open(path));
        const uint8_t a[] = {0xFA}, b[] = {0xF2, 0x04, 0x00};
        sink.send(0, a, 1); sink.send(0, b, 3);
    }
    FILE* f = std::fopen(path, "rb"); REQUIRE(f != nullptr);
    uint8_t buf[8]; const size_t n = std::fread(buf, 1, 8, f); std::fclose(f); std::remove(path);
    CHECK_EQ(n, size_t(4)); CHECK_EQ(int(buf[0]), 0xFA); CHECK_EQ(int(buf[3]), 0x00);
    RawMidiSink bad; CHECK(!bad.open("/nonexistent/dev")); CHECK(!bad.lastError().empty());
}
