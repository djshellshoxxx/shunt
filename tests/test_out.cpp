// RS-07 output tests O-T1 .. O-T6.
#include "TestFramework.h"
#include "shunt/out/Link.h"
#include "shunt/out/Midi.h"
#include "shunt/out/Osc.h"
#include <cstring>

using namespace shunt;
using namespace shunt::out;

namespace {
clock::Timeline tl120(bool barKnown = true) {
    clock::Timeline t;
    t.bpm = 120; t.beatOriginNs = 1'000'000'000; t.beatIndex = 10; t.beatInBar = 3;
    t.barKnown = barKnown; t.playing = true; t.resetSequence = 1; t.masterDevice = 1;
    return t;
}
std::vector<MidiMsg> run(MidiClockGenerator& g, clock::Timeline tl, int64_t from, int64_t to, int64_t step = 1'000'000) {
    std::vector<MidiMsg> all;
    for (int64_t now = from; now < to; now += step) g.poll(now, 3'000'000, tl, all);
    return all;
}
size_t count(const std::vector<MidiMsg>& v, uint8_t b) { size_t n = 0; for (auto& m : v) n += m.bytes[0] == b; return n; }
}

TEST_CASE("O-T1 OSC encoding is byte exact") {
    auto m = oscMessage("/shunt/bpm", {OscArg::Float(120.0f)});
    const uint8_t expect[] = {'/', 's', 'h', 'u', 'n', 't', '/', 'b', 'p', 'm', 0, 0, ',', 'f', 0, 0, 0x42, 0xf0, 0, 0};
    REQUIRE(m.size() == sizeof expect);
    CHECK(std::memcmp(m.data(), expect, sizeof expect) == 0);
    auto s = oscMessage("/a", {OscArg::Int(-1), OscArg::Str("hi")});
    const uint8_t e2[] = {'/', 'a', 0, 0, ',', 'i', 's', 0, 0xff, 0xff, 0xff, 0xff, 'h', 'i', 0, 0};
    REQUIRE(s.size() == sizeof e2);
    CHECK(std::memcmp(s.data(), e2, sizeof e2) == 0);
}

TEST_CASE("O-T2 MIDI clock at 120 BPM: 24 ppqn spacing, one Start on a bar boundary") {
    MidiClockGenerator g;
    auto msgs = run(g, tl120(), 1'000'000'000, 6'000'000'000);
    CHECK_EQ(count(msgs, 0xFA), 1u);
    size_t startIdx = 0;
    for (size_t i = 0; i < msgs.size(); ++i) if (msgs[i].bytes[0] == 0xFA) startIdx = i;
    // beat index 10 is beat 3: next bar boundary beats are 12 (inBar 1) -> 1 s + 2 beats * 0.5 s = 2.0 s
    CHECK_NEAR(double(msgs[startIdx].timeNs), 2.0e9, 1.0);
    std::vector<int64_t> clocks;
    for (auto& m : msgs) if (m.bytes[0] == 0xF8) clocks.push_back(m.timeNs);
    REQUIRE(clocks.size() > 100u);
    for (size_t i = 1; i < clocks.size(); ++i) CHECK_NEAR(double(clocks[i] - clocks[i - 1]), 1e9 / 48.0, 2.0);
    CHECK(msgs[startIdx].timeNs <= clocks.front());
}

TEST_CASE("O-T2b without bar knowledge Start waits only for a beat") {
    MidiClockGenerator g;
    auto msgs = run(g, tl120(false), 1'000'000'000, 3'000'000'000);
    size_t startIdx = 0;
    for (size_t i = 0; i < msgs.size(); ++i) if (msgs[i].bytes[0] == 0xFA) startIdx = i;
    CHECK(msgs[startIdx].timeNs < 1'600'000'000);
}

TEST_CASE("O-T3 pause: keep running, or Stop then Continue") {
    {
        MidiClockGenerator g;
        auto tl = tl120();
        auto a = run(g, tl, 1'000'000'000, 3'000'000'000);
        tl.playing = false;
        auto b = run(g, tl, 3'000'000'000, 5'000'000'000);
        CHECK_EQ(count(b, 0xFC), 0u);
        CHECK(count(b, 0xF8) > 90u);
        (void)a;
    }
    {
        MidiClockOptions o; o.pause = MidiClockOptions::Stop;
        MidiClockGenerator g; g.setOptions(o);
        auto tl = tl120();
        run(g, tl, 1'000'000'000, 3'000'000'000);
        tl.playing = false;
        auto b = run(g, tl, 3'000'000'000, 5'000'000'000);
        CHECK_EQ(count(b, 0xFC), 1u);
        CHECK_EQ(count(b, 0xF8), 0u);
        tl.playing = true;
        auto c = run(g, tl, 5'000'000'000, 7'000'000'000);
        CHECK_EQ(count(c, 0xFB), 1u);
        CHECK_EQ(count(c, 0xFA), 0u);
        CHECK(count(c, 0xF8) > 80u);
    }
}

TEST_CASE("O-T4 reset resyncs without Start unless enabled; SPP when enabled") {
    MidiClockOptions o; o.startOnReset = false; o.sppOnReset = true;
    MidiClockGenerator g; g.setOptions(o);
    auto tl = tl120();
    run(g, tl, 1'000'000'000, 3'000'000'000);
    tl.resetSequence = 2; tl.beatOriginNs = 3'250'000'000; tl.beatIndex = 20; tl.beatInBar = 2;
    auto b = run(g, tl, 3'000'000'000, 5'000'000'000);
    CHECK_EQ(count(b, 0xFA), 0u);
    CHECK_EQ(count(b, 0xF2), 1u);
    CHECK(count(b, 0xF8) > 80u);
    // ticks stay on the new phase grid
    for (auto& m : b) if (m.bytes[0] == 0xF8) {
        const double ticks = (double(m.timeNs) - 3.25e9) / (1e9 / 48.0);
        CHECK_NEAR(ticks, std::round(ticks), 0.01);
    }
}

namespace {
struct MockLink : ILinkSession {
    double bpm = 0; double phase = 0;   // link beat at t = phase + (t / 0.5 s)
    int soft = 0, hard = 0, tempoSets = 0;
    void enable(bool) override {}
    size_t numPeers() const override { return 2; }
    void setTempo(double b, int64_t) override { bpm = b; ++tempoSets; }
    double beatAtTime(int64_t t, double) const override { return phase + double(t) / 5e8; }
    void requestBeatAtTime(double, int64_t, double) override { ++soft; }
    void forceBeatAtTime(double, int64_t, double) override { ++hard; }
};
}

TEST_CASE("O-T5 Link logic: quantum, tempo deadband, soft and hard correction") {
    auto mock = std::make_unique<MockLink>();
    MockLink* m = mock.get();
    LinkOutput lo(std::move(mock));
    lo.setEnabled(true);
    auto tl = tl120();                      // beatInBar 3 at index 10 => link beat should be 2 mod 4 at t=1 s
    m->phase = 0.0;                         // link beat at 1 s = 2.0 -> in phase (index 10 -> inBar 3 -> target 2)
    for (int64_t now = 1'000'000'000; now < 6'000'000'000; now += 1'000'000) lo.poll(now, tl);
    CHECK_EQ(m->tempoSets, 1);
    CHECK_NEAR(m->bpm, 120.0, 1e-9);
    CHECK_EQ(m->hard, 1);                   // initial alignment on first lock (resetSequence 0 -> 1)
    CHECK_EQ(m->soft, 0);
    CHECK_NEAR(lo.lastQuantum(), 4.0, 1e-9);
    // 10 ms (0.02 beat) error persists -> soft correction after two beats
    m->phase = 0.02;
    for (int64_t now = 6'000'000'000; now < 9'000'000'000; now += 1'000'000) lo.poll(now, tl);
    CHECK(m->soft >= 1);
    CHECK_EQ(m->hard, 1);
    // reset -> hard
    tl.resetSequence = 2;
    for (int64_t now = 9'000'000'000; now < 11'000'000'000; now += 1'000'000) lo.poll(now, tl);
    CHECK(m->hard >= 2);
    // beat-only => quantum 1
    auto tl2 = tl120(false);
    LinkOutput lo2(std::make_unique<MockLink>());
    lo2.setEnabled(true);
    lo2.poll(1'000'000'000, tl2);
    CHECK_NEAR(lo2.lastQuantum(), 1.0, 1e-9);
}

TEST_CASE("O-T5b Link without SDK reports unavailable") {
    LinkOutput lo;
    lo.setEnabled(true);
    CHECK(!lo.available());
    CHECK(lo.error().find("SDK") != std::string::npos);
}

TEST_CASE("O-T6 OSC profiles") {
    auto run = [](OscProfile p) {
        OscOutput o;
        std::vector<std::vector<uint8_t>> sent;
        o.setSink([&](const std::vector<uint8_t>& b) { sent.push_back(b); });
        OscSettings s; s.enabled = true; s.profile = p;
        o.configure(s);
        auto tl = tl120();
        for (int64_t now = 1'000'000'000; now < 4'000'000'000; now += 1'000'000) o.poll(now, tl);
        return sent;
    };
    auto contains = [](const std::vector<std::vector<uint8_t>>& v, const std::string& addr) {
        for (auto& b : v) if (std::string(reinterpret_cast<const char*>(b.data())) == addr) return true;
        return false;
    };
    auto gen = run(OscProfile::Generic);
    CHECK(contains(gen, "/shunt/bpm"));
    CHECK(contains(gen, "/shunt/beat"));
    CHECK(contains(gen, "/shunt/bar"));
    auto res = run(OscProfile::Resolume);
    CHECK(contains(res, "/composition/tempocontroller/tempo"));
    CHECK(contains(res, "/composition/tempocontroller/resync"));
    auto ma = run(OscProfile::GrandMA3);
    REQUIRE(!ma.empty());
    std::string payload(reinterpret_cast<const char*>(ma[0].data()) + 16, 24);
    CHECK(payload.find("Master 3.1 At BPM 120.00") != std::string::npos);
    // Resolume normalisation (120-20)/480
    const auto& t = res[0];
    uint32_t u = (uint32_t(t[t.size() - 4]) << 24) | (uint32_t(t[t.size() - 3]) << 16) | (uint32_t(t[t.size() - 2]) << 8) | t[t.size() - 1];
    float f; std::memcpy(&f, &u, 4);
    CHECK_NEAR(double(f), 100.0 / 480.0, 1e-6);
    // QLab events
    OscOutput q; std::vector<std::string> addrs;
    q.setSink([&](const std::vector<uint8_t>& b) { addrs.emplace_back(reinterpret_cast<const char*>(b.data())); });
    OscSettings qs; qs.enabled = true; qs.profile = OscProfile::QLab; qs.qlabMasterCue = 7;
    q.configure(qs);
    q.onMasterChanged(2);
    REQUIRE(addrs.size() == 1u);
    CHECK(addrs[0] == "/cue/7/start");
}
