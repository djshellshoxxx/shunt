// FS-01 tests OA-T1..OA-T9 and O-T8: on-air aggregation, hold and mute, MIDI clock mute, OSC
// edges and QLab cues, settings/status API, the sendOsc hook, and the onair / onair-nomixer
// scenario timeline end to end (FixtureBuilder capture -> NetworkStack -> engine -> feature ->
// MidiClockGenerator) with synthetic time. Original Shunt code.
#include "TestFramework.h"
#include "FixtureBuilder.h"
#include "../../app/src/features/OnAir.h"
#include "shunt/app/Api.h"
#include "shunt/app/Core.h"
#include "shunt/net/NetworkStack.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <unistd.h>

using namespace shunt;
using namespace shunt::app;
using namespace shunt::app::onair;

namespace {
constexpr int64_t S = 1'000'000'000;
constexpr int64_t MS = 1'000'000;

template <class F> bool waitFor(F cond, int ms = 2000) {
    for (int i = 0; i < ms / 2; ++i) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return cond();
}

int countKind(const std::vector<Edge>& v, Edge::Kind k, bool value) {
    int n = 0;
    for (auto& e : v) n += e.kind == k && e.value == value;
    return n;
}

void mixer(OnAirAggregator& a, std::initializer_list<int> on, int64_t t) {
    uint8_t ch[6] = {0, 0, 0, 0, 0, 0};
    for (int c : on) ch[c - 1] = 1;
    a.onMixerChannels(ch, 4, t);
}

// Runs the aggregator from..to in 10 ms ticks with mixer packets every 200 ms carrying `on`.
std::vector<Edge> runMixer(OnAirAggregator& a, std::initializer_list<int> on, int64_t from, int64_t to) {
    std::vector<Edge> all;
    for (int64_t t = from; t < to; t += 10 * MS) {
        if ((t - from) % (200 * MS) == 0) mixer(a, on, t);
        auto e = a.tick(t);
        all.insert(all.end(), e.begin(), e.end());
    }
    return all;
}

OnAirConfig cfg(int64_t holdNs = 4 * S) { OnAirConfig c; c.holdNs = holdNs; return c; }

// Minimal FeatureHost: a real OutputManager (OSC to a sink), everything else stubbed.
struct FakeHost : FeatureHost {
    FakeHost() : outputs_([] { return clock::Timeline{}; }) {}
    int64_t wallNowMs() const override { return 1'700'000'000'000; }
    MetaStore& meta() override { return meta_; }
    ControlRegistry& controls() override { return controls_; }
    out::OutputManager& outputs() override { return outputs_; }
    Json settings(const std::string&) override { return settings_; }
    bool saveSettings(const std::string&, const Json&) override { return false; }
    const clock::ClockEngine& engine() const override { return engine_; }
    void onSessionCleared(std::function<void()>) override {}
    bool enterReplay(ReplayHooks*) override { return false; }
    void leaveReplay() override {}
    bool replaying() const override { return false; }
    bool feed(uint16_t, const uint8_t*, size_t, int64_t, uint32_t, int64_t) override { return false; }
    void resetEngine(bool) override {}
    void setOutputsEnabled(bool) override {}

    // Routes OSC to `sent` (output thread) and starts the output thread.
    void startOsc(out::OscProfile p) {
        outputs_.osc().setSink([this](const std::vector<uint8_t>& b) {
            std::lock_guard<std::mutex> lk(mu);
            sent.emplace_back(reinterpret_cast<const char*>(b.data()));
            raw.push_back(b);
        });
        out::OutputSettings os;
        os.osc.enabled = true;
        os.osc.profile = p;
        outputs_.configure(os);
        outputs_.start();
    }
    std::vector<std::string> addrs() { std::lock_guard<std::mutex> lk(mu); return sent; }
    size_t count(const std::string& a) {
        std::lock_guard<std::mutex> lk(mu);
        size_t n = 0;
        for (auto& s : sent) n += s == a;
        return n;
    }

    Json settings_ = Json::object();
    MetaStore meta_;
    ControlRegistry controls_;
    clock::ClockEngine engine_;
    out::OutputManager outputs_;
    std::mutex mu;
    std::vector<std::string> sent;
    std::vector<std::vector<uint8_t>> raw;
};

Json parse(const std::string& s) { Json j; Json::parse(s, j); return j; }

net::Event statusEv(uint8_t deck, bool onAir, int64_t t) {
    net::Event e;
    e.type = net::EventType::PlayerStatus;
    e.device = deck;
    e.recvTimeNs = t;
    e.status = net::PlayerStatusEvent{};
    e.status.onAir = onAir;
    return e;
}
net::Event mixerEv(std::initializer_list<int> on, int64_t t) {
    net::Event e;
    e.type = net::EventType::OnAir;
    e.device = 0x21;
    e.recvTimeNs = t;
    e.onAir = net::OnAirEvent{};
    e.onAir.channelCount = 4;
    for (int c : on) e.onAir.channels[c - 1] = 1;
    return e;
}

clock::Timeline tl120(bool barKnown = true) {
    clock::Timeline t;
    t.bpm = 120; t.beatOriginNs = 1 * S; t.beatIndex = 10; t.beatInBar = 3;
    t.barKnown = barKnown; t.playing = true; t.resetSequence = 1; t.masterDevice = 1;
    return t;
}
std::vector<out::MidiMsg> runGen(out::MidiClockGenerator& g, const clock::Timeline& tl, int64_t from, int64_t to) {
    std::vector<out::MidiMsg> all;
    for (int64_t now = from; now < to; now += MS) g.poll(now, 3 * MS, tl, all);
    return all;
}
size_t count(const std::vector<out::MidiMsg>& v, uint8_t b) { size_t n = 0; for (auto& m : v) n += m.bytes[0] == b; return n; }
const out::MidiMsg* first(const std::vector<out::MidiMsg>& v, uint8_t b) { for (auto& m : v) if (m.bytes[0] == b) return &m; return nullptr; }
int inBarAt(const clock::Timeline& tl, int64_t t) {   // 1..4 for the beat starting at t
    const int64_t beat = tl.beatIndex + int64_t(std::llround(double(t - tl.beatOriginNs) / tl.periodNs()));
    return int(((int64_t(tl.beatInBar - 1) + (beat - tl.beatIndex)) % 4 + 4) % 4) + 1;
}
} // namespace

// ---------------------------------------------------------------- aggregator

TEST_CASE("OA-T1 hold: 3.9 s no mute, 4.1 s mute, back on air unmutes at once") {
    OnAirAggregator a;
    a.configure(cfg());
    a.onPlayerBit(1, true, 0);
    auto e = runMixer(a, {1}, 0, 1 * S);
    CHECK(a.any());
    CHECK_EQ(countKind(e, Edge::AnyChanged, true), 1);
    CHECK_EQ(countKind(e, Edge::DeckChanged, true), 1);
    e = runMixer(a, {}, 1 * S, 1 * S + 3900 * MS);    // all channels off at t=1 s
    CHECK(!a.any());
    CHECK(!a.muted());
    CHECK_EQ(countKind(e, Edge::AnyChanged, false), 1);
    CHECK_EQ(countKind(e, Edge::MuteChanged, true), 0);
    e = runMixer(a, {}, 1 * S + 3900 * MS, 1 * S + 4100 * MS);
    CHECK(a.muted());
    CHECK_EQ(countKind(e, Edge::MuteChanged, true), 1);
    mixer(a, {2, 1}, 5200 * MS);                      // back on air: unmute in the same tick
    e = a.tick(5200 * MS);
    CHECK(!a.muted());
    CHECK(a.any());
    CHECK_EQ(countKind(e, Edge::MuteChanged, false), 1);
    CHECK_EQ(countKind(e, Edge::AnyChanged, true), 1);
    // crossfader cut: both decks off for less than the hold never mutes
    a.onPlayerBit(2, false, 5200 * MS);
    e = runMixer(a, {}, 6 * S, 9 * S);
    e = runMixer(a, {2}, 9 * S, 12 * S);
    CHECK_EQ(countKind(e, Edge::MuteChanged, true), 0);
    CHECK(!a.muted());
    // muteEnabled false (midiMute off): never mutes
    OnAirAggregator b;
    OnAirConfig c = cfg(); c.muteEnabled = false;
    b.configure(c);
    b.onPlayerBit(1, false, 0);
    runMixer(b, {}, 0, 20 * S);
    CHECK(!b.muted());
    CHECK(b.dataSeen());
}

TEST_CASE("OA-T2 source precedence: mixer authoritative while fresh, 2 s staleness falls back to bits") {
    OnAirAggregator a;
    a.configure(cfg());
    CHECK_STR(a.source(0), "none");
    a.onPlayerBit(1, true, 0);
    a.tick(0);
    CHECK(a.deckOn(1, 0));
    CHECK_STR(a.source(0), "players");
    mixer(a, {}, 100 * MS);                           // mixer says ch1 off: mixer wins
    a.tick(100 * MS);
    CHECK(!a.deckOn(1, 100 * MS));
    CHECK(!a.any());
    CHECK_STR(a.source(100 * MS), "mixer");
    CHECK(!a.deckOn(1, 2100 * MS));                   // exactly 2 s: still fresh
    auto e = a.tick(2101 * MS);                       // stale: player bit again
    CHECK(a.deckOn(1, 2101 * MS));
    CHECK(a.any());
    CHECK_STR(a.source(2101 * MS), "players");
    CHECK_EQ(countKind(e, Edge::AnyChanged, true), 1);
    // player bit off but mixer channel on: on air
    a.onPlayerBit(2, false, 3 * S);
    mixer(a, {2}, 3 * S);
    a.tick(3 * S);
    CHECK(a.deckOn(2, 3 * S));
    CHECK(!a.deckOn(1, 3 * S));
    CHECK(!a.deckOn(3, 3 * S));                       // not present
}

TEST_CASE("OA-T3 channel map remaps decks; wrong-length or bad maps fall back") {
    OnAirAggregator a;
    OnAirConfig c = cfg();
    c.channelMap = {{3, 1, 2, 4, 5, 6}};             // deck 1 is plugged into channel 3
    a.configure(c);
    a.onPlayerBit(1, false, 0);
    a.onPlayerBit(2, false, 0);
    mixer(a, {3}, 0);
    a.tick(0);
    CHECK(a.deckOn(1, 0));
    CHECK(!a.deckOn(2, 0));
    mixer(a, {1}, 10 * MS);
    a.tick(10 * MS);
    CHECK(!a.deckOn(1, 10 * MS));
    CHECK(a.deckOn(2, 10 * MS));
    // channels beyond the packet's count are off (4-channel mixer, deck mapped to ch 6)
    c.channelMap = {{6, 2, 3, 4, 5, 6}};
    a.configure(c);
    uint8_t all[6] = {1, 1, 1, 1, 1, 1};
    a.onMixerChannels(all, 4, 20 * MS);
    CHECK(!a.deckOn(1, 20 * MS));
    a.onMixerChannels(all, 6, 30 * MS);
    CHECK(a.deckOn(1, 30 * MS));
    a.onMixerChannels(all, 99, 40 * MS);              // count clamped
    CHECK(a.deckOn(1, 40 * MS));

    auto map = [](const char* json) { return OnAirSettings::fromJson(parse(std::string("{\"channelMap\":") + json + "}")).channelMap; };
    const std::array<uint8_t, 6> id{{1, 2, 3, 4, 5, 6}};
    CHECK(map("[1,2]") == id);
    CHECK(map("[1,2,3,4,5,6,1]") == id);
    CHECK(map("\"x\"") == id);
    CHECK(map("[2,1,3,4,5,6]") == (std::array<uint8_t, 6>{{2, 1, 3, 4, 5, 6}}));
    CHECK(map("[0,9,\"x\",-5,1e9,6]") == (std::array<uint8_t, 6>{{1, 6, 3, 1, 6, 6}}));
}

TEST_CASE("OA-T4 no on-air data ever, or no player: never mutes") {
    OnAirAggregator a;
    a.configure(cfg(500 * MS));
    a.onPlayerBit(1, false, 0);                       // pre-mixer setup: bits always 0
    a.onPlayerBit(2, false, 0);
    for (int64_t t = 0; t < 60 * S; t += 50 * MS) { a.onPlayerBit(1, false, t); a.tick(t); }
    CHECK(!a.dataSeen());
    CHECK(!a.muted());
    OnAirAggregator b;                                // mixer data but no player present
    b.configure(cfg(500 * MS));
    runMixer(b, {}, 0, 30 * S);
    CHECK(b.dataSeen());
    CHECK(!b.muted());
    b.onPlayerBit(1, false, 30 * S);                  // a player arrives: hold starts now
    runMixer(b, {}, 30 * S, 30 * S + 400 * MS);
    CHECK(!b.muted());
    runMixer(b, {}, 30 * S + 400 * MS, 31 * S);
    CHECK(b.muted());
}

TEST_CASE("OA-T5 deck leaves while it was the only deck on air") {
    OnAirAggregator a;
    a.configure(cfg());
    a.onPlayerBit(1, false, 0);
    a.onPlayerBit(2, true, 0);
    a.tick(0);
    CHECK(a.any());
    a.onDeckLeft(2, 1 * S);
    auto e = a.tick(1 * S);
    CHECK(!a.any());
    CHECK_EQ(countKind(e, Edge::AnyChanged, false), 1);
    CHECK_EQ(countKind(e, Edge::DeckChanged, false), 1);
    CHECK(!a.deckPresent(2));
    a.tick(4900 * MS);
    CHECK(!a.muted());
    e = a.tick(5 * S);
    CHECK(a.muted());
    a.onPlayerBit(2, true, 6 * S);                    // comes back on air
    e = a.tick(6 * S);
    CHECK(!a.muted());
    CHECK_EQ(countKind(e, Edge::MuteChanged, false), 1);
}

// ---------------------------------------------------------------- MIDI generator

TEST_CASE("O-T8 generator mute: one Stop, no clocks while muted, Start or Continue at a bar") {
    for (int mode = 0; mode < 2; ++mode) {
        out::MidiClockGenerator g;
        g.setUnmuteMode(mode ? out::UnmuteMode::Continue : out::UnmuteMode::Start);
        const auto tl = tl120();
        auto a = runGen(g, tl, 1 * S, 3 * S);
        CHECK_EQ(count(a, 0xFA), 1u);
        CHECK(g.running());
        g.setMuted(true);
        auto b = runGen(g, tl, 3 * S, 5 * S);
        CHECK_EQ(count(b, 0xFC), 1u);
        CHECK_EQ(count(b, 0xF8), 0u);
        CHECK_EQ(b.size(), 1u);
        CHECK(g.muted());
        CHECK(!g.running());
        g.setMuted(false);
        const int64_t unmuteAt = 5 * S + 100 * MS;
        auto pre = runGen(g, tl, 5 * S, unmuteAt);   // the flag is applied on the next poll
        auto c = runGen(g, tl, unmuteAt, 9 * S);
        c.insert(c.begin(), pre.begin(), pre.end());
        CHECK_EQ(count(c, 0xFC), 0u);
        CHECK_EQ(count(c, mode ? 0xFB : 0xFA), 1u);
        CHECK_EQ(count(c, mode ? 0xFA : 0xFB), 0u);
        const out::MidiMsg* s = first(c, mode ? 0xFB : 0xFA);
        REQUIRE(s != nullptr);
        CHECK_EQ(inBarAt(tl, s->timeNs), 1);
        CHECK(s->timeNs >= 5 * S && s->timeNs <= 5 * S + 2 * S);    // within one bar at 120 BPM
        const out::MidiMsg* clk = first(c, 0xF8);
        REQUIRE(clk != nullptr);
        CHECK(clk->timeNs >= s->timeNs);              // no clocks before the restart message
        CHECK(count(c, 0xF8) > 80u);
    }
    // bar unknown: next beat
    out::MidiClockGenerator g;
    const auto tl = tl120(false);
    runGen(g, tl, 1 * S, 2 * S);
    g.setMuted(true);
    runGen(g, tl, 2 * S, 3 * S);
    g.setMuted(false);
    auto c = runGen(g, tl, 3 * S + 10 * MS, 5 * S);
    const out::MidiMsg* s = first(c, 0xFA);
    REQUIRE(s != nullptr);
    CHECK(s->timeNs <= 3 * S + 10 * MS + 500 * MS);
    // muted while paused with pause=Stop: no second Stop; unmuting while still paused stays quiet,
    // playing again restarts per the unmute mode (Start), not with the pause path's Continue
    out::MidiClockOptions o; o.pause = out::MidiClockOptions::Stop;
    out::MidiClockGenerator p; p.setOptions(o);
    auto ptl = tl120();
    runGen(p, ptl, 1 * S, 3 * S);
    ptl.playing = false;
    auto d = runGen(p, ptl, 3 * S, 4 * S);
    CHECK_EQ(count(d, 0xFC), 1u);
    p.setMuted(true);
    d = runGen(p, ptl, 4 * S, 5 * S);
    CHECK_EQ(d.size(), 0u);
    p.setMuted(false);
    d = runGen(p, ptl, 5 * S, 6 * S);
    CHECK_EQ(d.size(), 0u);
    ptl.playing = true;
    d = runGen(p, ptl, 6 * S, 9 * S);
    CHECK_EQ(count(d, 0xFA), 1u);
    CHECK_EQ(count(d, 0xFB), 0u);
    CHECK_EQ(count(d, 0xFC), 0u);
    // muted before the first lock: nothing at all, then the normal first-lock Start
    out::MidiClockGenerator q;
    q.setMuted(true);
    auto e = runGen(q, tl120(), 1 * S, 2 * S);
    CHECK_EQ(e.size(), 0u);
    q.setMuted(false);
    e = runGen(q, tl120(), 2 * S, 5 * S);
    CHECK_EQ(count(e, 0xFA), 1u);
    CHECK_EQ(count(e, 0xFC), 0u);
}

// ---------------------------------------------------------------- feature, OSC, hook

TEST_CASE("OA-T6 OSC: per-deck, any and mute messages; QLab cues; other profiles silent") {
    auto drive = [](FakeHost& h, OnAirFeature& f) {
        for (int64_t t = 0; t < 1 * S; t += 10 * MS) {
            if (t % (200 * MS) == 0) { f.onEvent(statusEv(1, true, t), 0); f.onEvent(mixerEv({1}, t), 0); }
            f.onTick(t, 0);
        }
        for (int64_t t = 1 * S; t < 3 * S; t += 10 * MS) {      // off air for 2 s (hold 1 s)
            if (t % (200 * MS) == 0) { f.onEvent(statusEv(1, false, t), 0); f.onEvent(mixerEv({}, t), 0); }
            f.onTick(t, 0);
        }
        (void)h;
    };
    WITH_TIMEOUT(10000, {
        FakeHost h;
        h.startOsc(out::OscProfile::Generic);
        h.settings_ = parse(R"({"enabled":true,"midiMute":"stop","holdMs":1000})");
        OnAirFeature f;
        f.init(h, h.settings_);
        drive(h, f);
        CHECK(h.outputs().muteRequested());
        CHECK(waitFor([&] { return h.count("/shunt/mute") == 1; }));
        CHECK_EQ(h.count("/shunt/onair"), 2u);           // deck 1 on, deck 1 off
        CHECK_EQ(h.count("/shunt/onair/any"), 2u);
        {   // argument check: first /shunt/onair is deck 1 on
            std::lock_guard<std::mutex> lk(h.mu);
            for (auto& b : h.raw) {
                out::OscMessage m;
                if (!out::oscParse(b.data(), b.size(), m) || m.address != "/shunt/onair") continue;
                REQUIRE(m.args.size() == 2u);
                CHECK_EQ(m.args[0].i, 1);
                CHECK_EQ(m.args[1].i, 1);
                break;
            }
        }
        // legacy per-deck messages from Core are suppressed while the feature is enabled
        h.outputs().push({out::OutputEvent::OnAir, 3, 0, true, "", ""});
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK_EQ(h.count("/shunt/onair"), 2u);
        f.applySettings(parse(R"({"enabled":false})"));  // disabled: legacy back, unmuted
        CHECK(!h.outputs().muteRequested());
        h.outputs().push({out::OutputEvent::OnAir, 3, 0, true, "", ""});
        CHECK(waitFor([&] { return h.count("/shunt/onair") == 3; }));
        h.outputs().stop();
    });
    WITH_TIMEOUT(10000, {   // oscPerDeck false: no per-deck messages
        FakeHost h;
        h.startOsc(out::OscProfile::MagicQ);
        OnAirFeature f;
        f.init(h, parse(R"({"enabled":true,"midiMute":"stop","holdMs":1000,"oscPerDeck":false})"));
        drive(h, f);
        CHECK(waitFor([&] { return h.count("/shunt/mute") == 1; }));
        CHECK_EQ(h.count("/shunt/onair"), 0u);
        CHECK_EQ(h.count("/shunt/onair/any"), 2u);
        h.outputs().stop();
    });
    WITH_TIMEOUT(10000, {   // QLab: cues only
        FakeHost h;
        h.startOsc(out::OscProfile::QLab);
        OnAirFeature f;
        f.init(h, parse(R"({"enabled":true,"midiMute":"stop","holdMs":1000,"qlabOnAirCue":5,"qlabMuteCue":6})"));
        drive(h, f);
        CHECK(waitFor([&] { return h.count("/cue/6/start") == 1; }));
        CHECK_EQ(h.count("/cue/5/start"), 1u);
        auto a = h.addrs();
        CHECK_EQ(a.size(), 2u);
        h.outputs().stop();
    });
    WITH_TIMEOUT(10000, {   // Resolume (and grandMA3): nothing from this feature
        FakeHost h;
        h.startOsc(out::OscProfile::Resolume);
        OnAirFeature f;
        f.init(h, parse(R"({"enabled":true,"midiMute":"stop","holdMs":1000,"qlabOnAirCue":5,"qlabMuteCue":6})"));
        drive(h, f);
        CHECK(h.outputs().muteRequested());
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CHECK_EQ(h.addrs().size(), 0u);
        h.outputs().stop();
    });
    {   // midiMute off: no mute, no /shunt/mute
        FakeHost h;
        OnAirFeature f;
        f.init(h, parse(R"({"enabled":true,"holdMs":1000})"));
        drive(h, f);
        CHECK(!h.outputs().muteRequested());
        CHECK(!f.status().get("muted").asBool());
    }
}

TEST_CASE("OA-T7 API round trip: settings validation, status shape, routes, disabled is inert") {
    // validation without HTTP
    OnAirSettings d = OnAirSettings::fromJson(parse(R"({"holdMs":"x","midiMute":"loud","unmuteWith":7,"oscPerDeck":"no"})"));
    CHECK_EQ(d.holdMs, 4000);
    CHECK(!d.muteStop);
    CHECK(d.unmuteWith == out::UnmuteMode::Start);
    CHECK(d.oscPerDeck);
    CHECK_EQ(OnAirSettings::fromJson(parse(R"({"holdMs":1})")).holdMs, 500);
    CHECK_EQ(OnAirSettings::fromJson(parse(R"({"holdMs":1e12})")).holdMs, 30000);
    CHECK_EQ(OnAirSettings::fromJson(parse(R"({"holdMs":2500})")).holdMs, 2500);
    CHECK_EQ(OnAirSettings::fromJson(parse(R"({"qlabMuteCue":-4,"qlabOnAirCue":123456})")).qlabMuteCue, 0);
    CHECK_EQ(OnAirSettings::fromJson(parse(R"({"qlabOnAirCue":123456})")).qlabOnAirCue, 9999);
    CHECK(OnAirSettings::fromJson(parse(R"({"unmuteWith":"continue"})")).unmuteWith == out::UnmuteMode::Continue);
    CHECK(!OnAirSettings::fromJson(Json()).enabled);
    CHECK_STR(OnAirSettings{}.toJson().dump(),
              R"({"enabled":false,"holdMs":4000,"midiMute":"off","unmuteWith":"start","channelMap":[1,2,3,4,5,6],"oscPerDeck":true,"qlabOnAirCue":0,"qlabMuteCue":0})");

    struct Probe : IFeature {        // reads the Core's OutputManager flags
        std::string id() const override { return "oaprobe"; }
        void init(FeatureHost& h, const Json&) override { host = &h; }
        FeatureHost* host = nullptr;
    };
    auto tmp = TEMP_DIR();
    Settings s;
    s.interfaceName = "fs01-no-such-interface";
    Core core(s, tmp.file("config.json"), tmp.path());
    auto* probe = new Probe;
    core.addFeature(std::unique_ptr<IFeature>(probe));
    core.start();
    auto req = [&](const std::string& m, const std::string& path, const std::string& body = "") {
        HttpRequest r; r.method = m; r.path = path; r.body = body;
        return handleRequest(core, r);
    };
    Json st = core.status().get("features").get("onair");
    CHECK_JSON(st, "enabled", false);
    CHECK_JSON(st, "any", false);
    CHECK_JSON(st, "muted", false);
    CHECK_JSON(st, "source", "none");
    REQUIRE(probe->host != nullptr);
    CHECK(!probe->host->outputs().muteRequested());

    auto r = req("POST", "/api/config", R"({"features":{"onair":{"enabled":true,"holdMs":99999,"channelMap":[1,2],"midiMute":"bogus","unmuteWith":"continue"}}})");
    CHECK_EQ(r.status, 200);
    r = req("GET", "/api/f/onair/settings");
    CHECK_EQ(r.status, 200);
    Json js = parse(r.body);
    CHECK_JSON(js, "holdMs", 30000);
    CHECK_JSON(js, "midiMute", "off");
    CHECK_JSON(js, "unmuteWith", "continue");
    CHECK_JSON(js, "enabled", true);
    CHECK_EQ(js.get("channelMap").size(), 6u);
    CHECK_EQ(req("POST", "/api/f/onair/settings", "{}").status, 405);
    CHECK_EQ(req("GET", "/api/f/onair/nope").status, 404);
    CHECK_EQ(req("GET", "/api/f/onair/../settings").status, 404);

    st = core.status().get("features").get("onair");
    CHECK_JSON(st, "enabled", true);
    CHECK_JSON_TYPE(st, "any", Json::Bool);
    CHECK_JSON_TYPE(st, "muted", Json::Bool);
    CHECK_JSON_TYPE(st, "dataSeen", Json::Bool);
    CHECK_JSON_TYPE(st, "source", Json::String);
    CHECK_JSON_TYPE(st, "decks", Json::Array);
    CHECK_JSON_TYPE(st, "sinceMs", Json::Number);
    CHECK_JSON_TYPE(st, "error", Json::String);
    CHECK_EQ(st.size(), 8u);
    Json feats = parse(req("GET", "/api/features").body);
    bool listed = false;
    for (auto& f : feats.items()) listed = listed || (f.get("id").asString() == "onair" && f.get("enabled").asBool());
    CHECK(listed);
    // other settings survive (deep merge) and disabling restores the default output state
    r = req("POST", "/api/config", R"({"features":{"onair":{"enabled":false}}})");
    CHECK_EQ(r.status, 200);
    CHECK(!probe->host->outputs().muteRequested());
    CHECK_JSON(core.status().get("features").get("onair"), "enabled", false);
    core.stop();
}

TEST_CASE("OA-T8 sendOsc hook delivers through the sink; no-op while OSC is disabled") {
    WITH_TIMEOUT(10000, {
        out::OutputManager om([] { return clock::Timeline{}; });
        std::mutex mu;
        std::vector<out::OscMessage> got;
        om.osc().setSink([&](const std::vector<uint8_t>& b) {
            out::OscMessage m;
            if (out::oscParse(b.data(), b.size(), m)) { std::lock_guard<std::mutex> lk(mu); got.push_back(m); }
        });
        om.start();
        CHECK(!om.oscEnabled());
        CHECK(!om.sendOsc("/x/disabled", {out::OscArg::Int(1)}));     // OSC disabled: dropped
        out::OutputSettings os; os.osc.enabled = true; os.osc.profile = out::OscProfile::GrandMA3;
        om.configure(os);
        CHECK(om.oscEnabled());
        CHECK(om.oscProfile() == out::OscProfile::GrandMA3);
        CHECK(!om.sendOsc("no-slash"));
        CHECK(!om.sendOsc(""));
        CHECK(om.sendOsc("/x/key", {out::OscArg::Str("8A"), out::OscArg::Float(0.5f), out::OscArg::Int(-3)}));
        std::atomic<int> okFromOther{0};
        std::thread t([&] { for (int i = 0; i < 20; ++i) okFromOther += om.sendOsc("/x/thread", {out::OscArg::Int(i)}) ? 1 : 0; });
        t.join();
        CHECK(waitFor([&] { std::lock_guard<std::mutex> lk(mu); return got.size() == 21; }));
        {
            std::lock_guard<std::mutex> lk(mu);
            REQUIRE(!got.empty());
            CHECK_STR(got[0].address, "/x/key");
            REQUIRE(got[0].args.size() == 3u);
            CHECK_STR(got[0].args[0].s, "8A");
            CHECK_NEAR(got[0].args[1].f, 0.5, 1e-6);
            CHECK_EQ(got[0].args[2].i, -3);
            for (auto& m : got) CHECK(m.address != "/x/disabled");
        }
        CHECK_EQ(okFromOther.load(), 20);
        om.stop();
        int accepted = 0;                             // not running: queue bounded at 256
        for (int i = 0; i < 300; ++i) accepted += om.sendOsc("/x/q") ? 1 : 0;
        CHECK_EQ(accepted, 256);
        os.osc.enabled = false;
        om.configure(os);
        CHECK(!om.sendOsc("/x/q"));
    });
}

// ---------------------------------------------------------------- scenario end to end

namespace {
struct ScenarioResult {
    std::vector<out::MidiMsg> midi;
    int64_t t0 = 0;
    std::vector<std::pair<double, bool>> mutes;      // (s, muted) edges seen by the generator
    Json lastStatus;
    std::vector<int> restartInBar;                    // beat-in-bar of each Start/Continue after a mute
};

// FixtureBuilder version of tools/shuntsim/scenarios/onair.cpp (same timeline, 128 BPM).
fixture::CaptureParams onairScenario(bool mixerOnAir) {
    fixture::CaptureParams p;
    p.seed = 3;
    p.durationS = 45;
    p.mixerOnAir = mixerOnAir;
    fixture::DeckSpec a; a.number = 1; a.master = true; a.onAir = true;
    fixture::DeckSpec b; b.number = 2; b.onAir = false;   // cued, playing under the fader
    p.decks = {a, b};
    p.changes.push_back({10.0, 1, [](fixture::DeckSpec& d) { d.onAir = false; }});
    p.changes.push_back({10.0, 2, [](fixture::DeckSpec& d) { d.onAir = true; }});
    p.changes.push_back({12.0, 1, [](fixture::DeckSpec& d) { d.master = false; }});
    p.changes.push_back({12.0, 2, [](fixture::DeckSpec& d) { d.master = true; }});
    p.changes.push_back({30.0, 2, [](fixture::DeckSpec& d) { d.onAir = false; }});
    p.changes.push_back({36.0, 2, [](fixture::DeckSpec& d) { d.onAir = true; }});
    return p;
}

ScenarioResult runScenario(bool mixerOnAir, const char* settings) {
    ScenarioResult r;
    const auto caps = fixture::buildCapture(onairScenario(mixerOnAir));
    auto factory = net::makePosixSocketFactory();
    net::StackConfig sc;
    sc.mode = net::Mode::Follow;            // master from status flags (passive infers it from beats)
    sc.filterSubnet = false;
    net::NetworkStack stack(*factory, sc);            // never started: no sockets
    clock::ClockEngine engine;
    FakeHost host;
    OnAirFeature f;
    f.init(host, parse(settings));
    out::MidiClockGenerator gen;
    out::MidiClockOptions mo; mo.startOnFirstLock = true;
    gen.setOptions(mo);
    r.t0 = caps.front().timestampNs;
    const int64_t end = r.t0 + 45 * S;
    size_t i = 0;
    bool lastMute = false;
    for (int64_t now = r.t0; now < end; now += MS) {
        while (i < caps.size() && caps[i].timestampNs <= now) {
            const auto& d = caps[i++];
            stack.feed(d.dstPort, d.payload.data(), d.payload.size(), d.timestampNs, d.srcIp);
        }
        stack.runTimers(now);
        net::Event ev;
        while (stack.pop(ev)) { engine.onEvent(ev); f.onEvent(ev, 0); }
        engine.tick(now);
        f.onTick(now, 0);
        const bool m = host.outputs().muteRequested();
        if (m != lastMute) { lastMute = m; r.mutes.push_back({double(now - r.t0) / 1e9, m}); }
        gen.setMuted(m);
        gen.setUnmuteMode(OnAirSettings::fromJson(parse(settings)).unmuteWith);
        const size_t before = r.midi.size();
        gen.poll(now, 3 * MS, engine.timeline(), r.midi);
        for (size_t k = before; k < r.midi.size(); ++k)
            if (!r.mutes.empty() && (r.midi[k].bytes[0] == 0xFA || r.midi[k].bytes[0] == 0xFB))
                r.restartInBar.push_back(engine.timeline().barKnown ? inBarAt(engine.timeline(), r.midi[k].timeNs) : 0);
    }
    r.lastStatus = f.status();
    return r;
}

void checkScenario(const ScenarioResult& r, uint8_t restart) {
    auto at = [&](const out::MidiMsg& m) { return double(m.timeNs - r.t0) / 1e9; };
    std::vector<double> stops, starts;
    for (auto& m : r.midi) {
        if (m.bytes[0] == 0xFC) stops.push_back(at(m));
        if (m.bytes[0] == restart) starts.push_back(at(m));
    }
    REQUIRE(stops.size() == 1u);
    REQUIRE(starts.size() == 2u);                    // first lock, then the unmute
    REQUIRE(r.mutes.size() == 2u);
    CHECK(r.mutes[0].second && !r.mutes[1].second);
    CHECK_NEAR(r.mutes[0].first, 34.0, 0.4);         // all off at 30 s + 4 s hold
    CHECK_NEAR(stops[0], 34.0, 0.4);
    CHECK(r.mutes[1].first >= 36.0 && r.mutes[1].first < 36.5);
    const double period = 60.0 / 128.0;
    CHECK(starts[1] >= r.mutes[1].first && starts[1] <= r.mutes[1].first + 4 * period + 0.01);   // next bar
    size_t clocksWhileMuted = 0, clocksAfter = 0;
    for (auto& m : r.midi) {
        if (m.bytes[0] != 0xF8) continue;
        const double t = at(m);
        if (t > stops[0] && t < starts[1]) ++clocksWhileMuted;
        if (t >= starts[1]) ++clocksAfter;
    }
    CHECK_EQ(clocksWhileMuted, 0u);
    CHECK(clocksAfter > 24u * 8u * 2u);               // > 8 s at 24 ppqn and 2 beats/s
    REQUIRE(r.restartInBar.size() == 1u);
    CHECK_EQ(r.restartInBar[0], 1);                   // on a downbeat
    // the first clock after the restart lands with it (the bar position itself is checked in O-T8)
    for (auto& m : r.midi) if (m.bytes[0] == 0xF8 && at(m) >= starts[1]) {
        CHECK_NEAR(at(m), starts[1], 0.002);          // first clock lands with the restart message
        break;
    }
}
} // namespace

TEST_CASE("OA-T9 scenario onair: clock stops at about 34 s and restarts at the next bar after 36 s") {
    WITH_TIMEOUT(60000, {
        const auto r = runScenario(true, R"({"enabled":true,"midiMute":"stop"})");
        checkScenario(r, 0xFA);
        CHECK_JSON(r.lastStatus, "source", "mixer");
        CHECK_JSON(r.lastStatus, "any", true);
        CHECK_JSON(r.lastStatus, "dataSeen", true);
        REQUIRE(r.lastStatus.get("decks").size() == 2u);
        CHECK_JSON(r.lastStatus.get("decks").items()[1], "on", true);
    });
}

TEST_CASE("OA-T9b scenario onair-nomixer (player bits only) and Continue; disabled policy is inert") {
    WITH_TIMEOUT(60000, {
        const auto r = runScenario(false, R"({"enabled":true,"midiMute":"stop","unmuteWith":"continue"})");
        size_t fa = 0;
        for (auto& m : r.midi) fa += m.bytes[0] == 0xFA;
        CHECK_EQ(fa, 1u);                             // only the first-lock Start
        // reuse the checks with Continue as the restart (first-lock Start counted separately)
        std::vector<double> cont;
        for (auto& m : r.midi) if (m.bytes[0] == 0xFB) cont.push_back(double(m.timeNs - r.t0) / 1e9);
        REQUIRE(cont.size() == 1u);
        REQUIRE(r.mutes.size() == 2u);
        CHECK_NEAR(r.mutes[0].first, 34.0, 0.4);
        CHECK(cont[0] >= r.mutes[1].first && cont[0] <= r.mutes[1].first + 4 * 60.0 / 128.0 + 0.01);
        CHECK_JSON(r.lastStatus, "source", "players");
        REQUIRE(r.restartInBar.size() == 1u);
        CHECK_EQ(r.restartInBar[0], 1);

        const auto off = runScenario(true, R"({"enabled":true,"midiMute":"off"})");
        size_t stops = 0;
        for (auto& m : off.midi) stops += m.bytes[0] == 0xFC;
        CHECK_EQ(stops, 0u);
        CHECK(off.mutes.empty());
    });
}

TEST_CASE("OA-T10 onTick budget: under 20 us per call (10x CI slack)") {
    FakeHost h;
    OnAirFeature f;
    f.init(h, parse(R"({"enabled":true,"midiMute":"stop"})"));
    for (int d = 1; d <= 6; ++d) f.onEvent(statusEv(uint8_t(d), d == 2, 0), 0);
    f.onEvent(mixerEv({2}, 0), 0);
    const int n = 20000;
    const auto a = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) f.onTick(int64_t(i) * 5 * MS / 100, 0);
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - a).count() / n;
    CHECK(us < 200.0);
}
