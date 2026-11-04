// FS-06 tests N-T1..N-T6: offset maths, rejections, tap latency compensation, undo, nudge
// clamping and actions, confidence JSON; plus a Core/HTTP integration case. The rig replays a
// FixtureBuilder capture (master deck reporting a wrong bar) through NetworkStack -> ClockEngine ->
// NudgeFeature with synthetic time; the "player-side" actions (bar offset, latency) are fakes that
// change the rig's engine config exactly like Core does. N-T7 (UI smoke) is tests/features/nudge_ui_test.js.
// Original Shunt code.
#include "TestFramework.h"
#include "FixtureBuilder.h"
#include "../../app/src/features/Nudge.h"
#include "shunt/app/Api.h"
#include "shunt/app/Core.h"
#include "shunt/net/NetworkStack.h"
#include <cmath>

using namespace shunt;
using namespace shunt::app;
using namespace shunt::app::nudge;

namespace {
constexpr int64_t S = 1'000'000'000;
constexpr int64_t MS = 1'000'000;

Json parse(const std::string& s) { Json j; Json::parse(s, j); return j; }

struct FakeHost : FeatureHost {
    FakeHost() : outputs_([] { return clock::Timeline{}; }) {}
    int64_t wallNowMs() const override { return 1'700'000'000'000; }
    MetaStore& meta() override { return meta_; }
    ControlRegistry& controls() override { return controls_; }
    out::OutputManager& outputs() override { return outputs_; }
    Json settings(const std::string&) override { return Json::object(); }
    bool saveSettings(const std::string&, const Json&) override { return false; }
    const clock::ClockEngine& engine() const override { return engine_; }
    void onSessionCleared(std::function<void()>) override {}
    bool enterReplay(ReplayHooks*) override { return false; }
    void leaveReplay() override {}
    bool replaying() const override { return false; }
    bool feed(uint16_t, const uint8_t*, size_t, int64_t, uint32_t, int64_t) override { return false; }
    void resetEngine(bool) override {}
    void setOutputsEnabled(bool) override {}

    void act(const std::string& id, std::function<void()> fn) {
        ControlAction a; a.id = id; a.label = id; a.fn = [fn](double) { fn(); };
        controls_.add(a);
    }
    // The fakes Core registers: baroffset.set.N, latency.up/down (0.5 ms, clamp), optionally latency.set.
    void installPlayerActions(bool latencySet) {
        for (int i = 0; i < 4; ++i) act("baroffset.set." + std::to_string(i), [this, i] { auto c = engine_.config(); c.userBarOffset = i; engine_.setConfig(c); ++barCalls; });
        auto lat = [this](double d) {
            auto c = engine_.config();
            double ms = std::round(std::clamp(c.latencyOffsetNs / 1e6 + d, -20.0, 50.0) * 10) / 10;
            c.latencyOffsetNs = int64_t(std::llround(ms * 1e6));
            engine_.setConfig(c);
        };
        act("latency.up", [lat] { lat(0.5); });
        act("latency.down", [lat] { lat(-0.5); });
        if (latencySet) {
            ControlAction a; a.id = "latency.set"; a.label = "x"; a.kind = ControlAction::Value;
            a.fn = [this](double v) {
                auto c = engine_.config();
                c.latencyOffsetNs = int64_t(std::llround((std::round((v * 70.0 - 20.0) * 10) / 10) * 1e6));   // Core would round the same way
                engine_.setConfig(c);
            };
            controls_.add(a);
        }
    }
    double latencyMs() const { return engine_.config().latencyOffsetNs / 1e6; }
    int barOffset() const { return engine_.config().userBarOffset; }

    MetaStore meta_;
    ControlRegistry controls_;
    clock::ClockEngine engine_;
    out::OutputManager outputs_;
    int barCalls = 0;
};

fixture::CaptureParams wrongBarCapture(double jitterMs = 0.3, int startBeatInBar = 3) {
    fixture::CaptureParams p;
    p.seed = 6;
    p.durationS = 40;
    p.mixer = false;
    p.jitterMs = jitterMs;
    fixture::DeckSpec d; d.number = 1; d.master = true; d.beatInBar = startBeatInBar;
    p.decks = {d};
    return p;
}

struct Rig {
    explicit Rig(fixture::CaptureParams p, const char* settings = "{}", bool latencySet = false)
        : caps(fixture::buildCapture(p)), factory(net::makePosixSocketFactory()), stack(*factory, cfg()) {
        host.installPlayerActions(latencySet);
        feature.setClock([this] { return now; });
        feature.init(host, parse(settings));
        t0 = caps.front().timestampNs;
        now = t0;
    }
    static net::StackConfig cfg() { net::StackConfig c; c.mode = net::Mode::Follow; c.filterSubnet = false; return c; }
    // Advances synthetic time to `to` (1 ms steps).
    void run(int64_t to) {
        for (; now < to; now += MS) {
            while (i < caps.size() && caps[i].timestampNs <= now) {
                const auto& d = caps[i++];
                stack.feed(d.dstPort, d.payload.data(), d.payload.size(), d.timestampNs, d.srcIp);
            }
            stack.runTimers(now);
            net::Event ev;
            while (stack.pop(ev)) { host.engine_.onEvent(ev); feature.onEvent(ev, 0); }
            host.engine_.tick(now);
            feature.onTick(now, 1'700'000'000'000 + (now - t0) / MS);
        }
    }
    HttpResponse post(const std::string& sub, const std::string& body = "{}") {
        HttpRequest r; r.method = "POST"; r.path = "/api/f/nudge/" + sub; r.body = body;
        auto x = feature.handle(r, sub);
        REQUIRE(x.has_value());
        return *x;
    }
    std::vector<net::CapturedDatagram> caps;
    std::unique_ptr<net::ISocketFactory> factory;
    net::NetworkStack stack;
    FakeHost host;
    NudgeFeature feature;
    size_t i = 0;
    int64_t t0 = 0, now = 0;
};
} // namespace

TEST_CASE("N-T1 offset maths: tap on a beat makes it beat 1 for every userOffset and raw (exhaustive, 16 cases)") {
    for (int off = 0; off < 4; ++off)
        for (int raw = 1; raw <= 4; ++raw) {
            clock::Timeline tl;
            tl.bpm = 120; tl.beatOriginNs = 1 * S; tl.beatIndex = 100; tl.barKnown = true;
            tl.beatInBar = (raw - 1 + off) % 4 + 1;                 // as the engine publishes it
            const TapGeometry g = tapGeometry(tl, off, tl.beatOriginNs);
            CHECK_EQ(g.k, 100);
            CHECK_EQ(g.raw, raw);
            CHECK(g.newOffset >= 0 && g.newOffset <= 3);
            CHECK_EQ((raw - 1 + g.newOffset) % 4 + 1, 1);
            // a later beat on the same timeline
            const TapGeometry g3 = tapGeometry(tl, off, tl.timeOfBeat(103));
            CHECK_EQ(g3.k, 103);
            CHECK_EQ(g3.raw, (raw - 1 + 3) % 4 + 1);
            CHECK_EQ((g3.raw - 1 + g3.newOffset) % 4 + 1, 1);
        }
    CHECK_EQ(offsetForRaw(1), 0);
    CHECK_EQ(offsetForRaw(2), 3);
    CHECK_EQ(offsetForRaw(4), 1);
}

TEST_CASE("N-T1b wrongbar: one tap fixes the bar lamp within one beat") {
    WITH_TIMEOUT(20000, {
        Rig r(wrongBarCapture());
        r.run(r.t0 + 8 * S);
        const clock::Timeline tl = r.host.engine_.timeline();
        REQUIRE(tl.barKnown);
        const int64_t k = tl.beatIndex + 3;                       // the beat the user "hears as one"
        r.run(tl.timeOfBeat(k) + 60 * MS);                        // tap 60 ms late (finger/network delay)
        HttpResponse x = r.post("tap");
        CHECK_EQ(x.status, 200);
        Json j = parse(x.body);
        CHECK_JSON(j, "ok", true);
        CHECK_EQ(r.host.barOffset(), j.get("offset").asInt());
        CHECK_EQ(j.get("previous").asInt(), 0);
        CHECK(j.get("distanceBeats").asNumber() < 0.05);
        // from the next published beat on, beat k + 4n is bar beat 1 and nothing else is
        const uint32_t seq0 = r.host.engine_.timeline().sequence;
        int checked = 0, ones = 0;
        int64_t lastIdx = -1;
        for (int64_t end = r.now + 4 * S; r.now < end;) {
            r.run(r.now + MS);
            const clock::Timeline& t = r.host.engine_.timeline();
            if (t.sequence != seq0 && t.beatIndex != lastIdx) {
                lastIdx = t.beatIndex;
                if (t.beatIndex >= k) {
                    ++checked;
                    const bool shouldBeOne = ((t.beatIndex - k) % 4 + 4) % 4 == 0;
                    CHECK_EQ(t.beatInBar == 1, shouldBeOne);
                    ones += t.beatInBar == 1;
                }
            }
        }
        CHECK(checked >= 6);
        CHECK(ones >= 1);
    });
}

TEST_CASE("N-T2 rejections: no master, beat only, low confidence, far from beat, rate limit") {
    WITH_TIMEOUT(20000, {
        // pure reasons
        clock::Timeline tl; tl.bpm = 128; tl.masterDevice = 1; tl.barKnown = true; tl.confidence = 0.9f;
        CHECK_STR(tapBlockReason(tl, clock::State::Locked, false, true, 0), "");
        CHECK_STR(tapBlockReason(tl, clock::State::Coasting, false, true, 0), "");
        CHECK_STR(tapBlockReason(tl, clock::State::Locked, false, false, 0), "no master");
        clock::Timeline none = tl; none.masterDevice = 0;
        CHECK_STR(tapBlockReason(none, clock::State::Locked, false, true, 0), "no master");
        CHECK_STR(tapBlockReason(tl, clock::State::Locked, true, true, 0), "bar not available: turn off beat only");
        clock::Timeline nobar = tl; nobar.barKnown = false;
        CHECK_STR(tapBlockReason(nobar, clock::State::Locked, false, true, 0), "bar not available for this player");
        CHECK_STR(tapBlockReason(tl, clock::State::Paused, false, true, 0), "not locked yet");
        CHECK_STR(tapBlockReason(tl, clock::State::Locking, false, true, 0), "not locked yet");
        clock::Timeline low = tl; low.confidence = 0.49f;
        CHECK_STR(tapBlockReason(low, clock::State::Locked, false, true, 0), "confidence too low");
        low.confidence = 0.5f;
        CHECK_STR(tapBlockReason(low, clock::State::Locked, false, true, 0), "");
        CHECK_STR(tapBlockReason(tl, clock::State::Locked, false, true, 2 * S), "no timing data");

        Rig r(wrongBarCapture());
        // before any packet: no master
        r.run(r.t0 + 50 * MS);
        HttpResponse x = r.post("tap");
        CHECK_EQ(x.status, 409);
        CHECK_JSON(parse(x.body), "ok", false);
        CHECK_JSON(parse(x.body), "error", "no master");
        CHECK_EQ(r.host.barCalls, 0);

        r.run(r.t0 + 8 * S);
        // beat-only
        auto c = r.host.engine_.config(); c.beatOnlyMode = true; r.host.engine_.setConfig(c);
        r.run(r.now + 400 * MS);
        r.now += 400 * MS;
        x = r.post("tap");
        CHECK_EQ(x.status, 409);
        CHECK_JSON(parse(x.body), "error", "bar not available: turn off beat only");
        c.beatOnlyMode = false; r.host.engine_.setConfig(c);
        r.run(r.now + 700 * MS);

        // far from a beat: half a period after a beat (tap latency compensation included)
        clock::Timeline t = r.host.engine_.timeline();
        int64_t k = t.beatIndex + 3;
        int64_t half = int64_t(t.periodNs() / 2);
        r.run(t.timeOfBeat(k) + half + 60 * MS);
        x = r.post("tap");
        CHECK_EQ(x.status, 409);
        CHECK_JSON(parse(x.body), "error", "tap closer to a beat");
        CHECK_EQ(r.host.barCalls, 0);

        // rate limit: a good tap, then another 100 ms later is ignored, one 400 ms later is taken
        t = r.host.engine_.timeline();
        k = t.beatIndex + 3;
        r.run(t.timeOfBeat(k) + 60 * MS);
        CHECK_EQ(r.post("tap").status, 200);
        const int calls = r.host.barCalls;
        r.run(r.now + 100 * MS);
        x = r.post("tap");
        CHECK_EQ(x.status, 429);
        CHECK_JSON(parse(x.body), "ignored", true);
        CHECK_EQ(r.host.barCalls, calls);
        // the ignored tap does not extend the window
        r.run(r.now + 250 * MS);
        t = r.host.engine_.timeline();
        {
            const int64_t kk = t.beatIndex + 3;
            r.run(t.timeOfBeat(kk) + 60 * MS);
            CHECK_EQ(r.post("tap").status, 200);
        }
        // stale snapshot (no ticks for 2 s)
        r.now += 2 * S;
        x = r.post("tap");
        CHECK_EQ(x.status, 409);
        CHECK_JSON(parse(x.body), "error", "no timing data");
        // wrong method and unknown route
        HttpRequest g; g.method = "GET"; g.path = "/api/f/nudge/tap";
        CHECK_EQ(r.feature.handle(g, "tap")->status, 405);
        CHECK(!r.feature.handle(g, "nope").has_value());
    });
}

TEST_CASE("N-T3 tap latency compensation: a tap 60 ms after a beat resolves to that beat") {
    WITH_TIMEOUT(20000, {
        for (int lat : {60, 0}) {
            Rig r(wrongBarCapture(0.0), lat == 60 ? R"({"tapLatencyMs":60})" : R"({"tapLatencyMs":0})");
            r.run(r.t0 + 8 * S);
            const clock::Timeline t = r.host.engine_.timeline();
            const int64_t k = t.beatIndex + 3;
            r.run(t.timeOfBeat(k) + 60 * MS);
            Json j = parse(r.post("tap").body);
            CHECK_JSON(j, "ok", true);
            const double expect = lat == 60 ? 0.0 : 60e6 / t.periodNs();
            CHECK_NEAR(j.get("distanceBeats").asNumber(), expect, 0.02);
        }
        // a late tap beyond half a period on 0 latency would resolve to the next beat; with the default 60 ms it does not
        Rig r(wrongBarCapture(0.0));
        r.run(r.t0 + 8 * S);
        const clock::Timeline t = r.host.engine_.timeline();
        const int64_t k = t.beatIndex + 3;
        r.run(t.timeOfBeat(k) + int64_t(0.5 * t.periodNs()) + 60 * MS - 5 * MS);   // 0.5 beat - 5 ms after beat k minus latency: still near k+0.5 -> rejected
        CHECK_EQ(r.post("tap").status, 409);
        // settings clamp
        CHECK_EQ(NudgeSettings::fromJson(parse(R"({"tapLatencyMs":999})")).tapLatencyMs, 250);
        CHECK_EQ(NudgeSettings::fromJson(parse(R"({"tapLatencyMs":-5})")).tapLatencyMs, 0);
        CHECK_EQ(NudgeSettings::fromJson(parse(R"({"tapLatencyMs":"x"})")).tapLatencyMs, 60);
        CHECK(NudgeSettings::fromJson(Json()).enabled);
        CHECK_STR(NudgeSettings{}.toJson().dump(), R"({"enabled":true,"tapLatencyMs":60,"wakeLock":true,"haptics":true})");
    });
}

TEST_CASE("N-T4 undo semantics and expiry") {
    WITH_TIMEOUT(20000, {
        Rig r(wrongBarCapture());
        r.run(r.t0 + 8 * S);
        CHECK_EQ(r.post("undo").status, 409);                      // nothing yet
        auto tapAt = [&](int beats) {
            const clock::Timeline t = r.host.engine_.timeline();
            r.run(t.timeOfBeat(t.beatIndex + beats) + 60 * MS);
            return r.post("tap");
        };
        HttpResponse x = tapAt(3);
        REQUIRE(x.status == 200);
        const int newOff = parse(x.body).get("offset").asInt();
        CHECK_EQ(r.host.barOffset(), newOff);
        x = r.post("undo");
        CHECK_EQ(x.status, 200);
        CHECK_JSON(parse(x.body), "offset", 0);
        CHECK_EQ(r.host.barOffset(), 0);
        CHECK_EQ(r.post("undo").status, 409);                      // single level
        // expiry after 60 s
        r.run(r.now + 1 * S);
        x = tapAt(3);
        REQUIRE(x.status == 200);
        r.now += 61 * S;                                           // jump the synthetic clock past the expiry
        r.feature.onTick(r.now, 0);
        CHECK_EQ(r.post("undo").status, 409);
        // track change clears undo
        Rig r2(wrongBarCapture());
        r2.run(r2.t0 + 8 * S);
        {
            const clock::Timeline t = r2.host.engine_.timeline();
            r2.run(t.timeOfBeat(t.beatIndex + 3) + 60 * MS);
            REQUIRE(r2.post("tap").status == 200);
            net::Event ev;
            ev.type = net::EventType::PlayerStatus;
            ev.device = 1;
            ev.recvTimeNs = r2.now;
            ev.status = net::PlayerStatusEvent{};
            ev.status.rekordboxId = 4242;
            ev.status.slot = 3;
            r2.feature.onEvent(ev, 0);
            ev.status.rekordboxId = 4243;                         // a different track
            r2.feature.onEvent(ev, 0);
            CHECK_EQ(r2.post("undo").status, 409);
            CHECK_JSON(r2.feature.status(), "canUndo", false);
        }
    });
}

TEST_CASE("N-T5 nudge clamping, rounding, log and actions (fallback 0.5 steps and latency.set)") {
    WITH_TIMEOUT(20000, {
        for (bool exact : {true, false}) {
            Rig r(wrongBarCapture(), "{}", exact);
            r.run(r.t0 + 3 * S);
            REQUIRE(r.host.latencyMs() == 1.0);
            // all actions are registered and bindable
            for (const char* id : {"nudge.plus", "nudge.minus", "nudge.fine.plus", "nudge.fine.minus", "nudge.reset", "downbeat.tap", "downbeat.undo"})
                CHECK(r.host.controls().has(id));
            Json j = parse(r.post("step", R"({"ms":0.5})").body);
            CHECK_JSON(j, "ok", true);
            CHECK_NEAR(r.host.latencyMs(), 1.5, 1e-9);
            // out-of-range request is clamped to +-5
            r.post("step", R"({"ms":99})");
            CHECK_NEAR(r.host.latencyMs(), 6.5, 1e-9);
            r.post("step", R"({"ms":-99})");
            CHECK_NEAR(r.host.latencyMs(), 1.5, 1e-9);
            // global limits
            for (int n = 0; n < 12; ++n) r.post("step", R"({"ms":5})");
            CHECK_NEAR(r.host.latencyMs(), 50.0, 1e-9);
            for (int n = 0; n < 20; ++n) r.post("step", R"({"ms":-5})");
            CHECK_NEAR(r.host.latencyMs(), -20.0, 1e-9);
            CHECK_EQ(r.post("step", "{}").status, 400);
            CHECK_EQ(r.post("step", R"({"ms":"x"})").status, 400);
            CHECK_EQ(r.post("step", "nope").status, 400);
            r.run(r.now + 20 * MS);
            // log: rolling, newest first, capped at 20
            for (int n = 0; n < 30; ++n) { r.post("step", n % 2 ? R"({"ms":1})" : R"({"ms":-1})"); r.run(r.now + 6 * MS); }
            Json st = r.feature.status();
            CHECK_EQ(st.get("log").size(), 20u);
            CHECK(st.get("log").items()[0].get("t").asNumber() > st.get("log").items()[19].get("t").asNumber());
            // reset returns to the value seen at start (1.0)
            r.post("reset");
            if (exact) CHECK_NEAR(r.host.latencyMs(), 1.0, 1e-9);
            else CHECK(std::fabs(r.host.latencyMs() - 1.0) <= 0.25 + 1e-9);

            // actions step through the registry
            const double before = r.host.latencyMs();
            r.run(r.now + 6 * MS);
            r.host.controls().invoke("nudge.plus", 1);
            CHECK_NEAR(r.host.latencyMs(), before + 0.5, 1e-9);
            r.run(r.now + 6 * MS);
            r.host.controls().invoke("nudge.minus", 1);
            CHECK_NEAR(r.host.latencyMs(), before, 1e-9);
            r.run(r.now + 6 * MS);
            if (exact) {
                r.host.controls().invoke("nudge.fine.plus", 1);
                CHECK_NEAR(r.host.latencyMs(), before + 0.1, 1e-9);
                r.run(r.now + 6 * MS);
                r.host.controls().invoke("nudge.fine.minus", 1);
                CHECK_NEAR(r.host.latencyMs(), before, 1e-9);
                CHECK_JSON(r.feature.status(), "fine", true);
            } else {                                              // five 0.1 presses add up to one 0.5 step
                for (int n = 0; n < 5; ++n) { r.host.controls().invoke("nudge.fine.plus", 1); r.run(r.now + 6 * MS); }
                CHECK_NEAR(r.host.latencyMs(), before + 0.5, 1e-9);
                CHECK_JSON(r.feature.status(), "fine", false);
            }
            // downbeat actions are bindable and share the tap rules (no tap possible here: just no crash, bar offset unchanged)
            r.host.controls().invoke("downbeat.undo", 1);
            CHECK_EQ(r.host.barOffset(), 0);
        }
        // disabled: actions and routes do nothing
        Rig d(wrongBarCapture(), R"({"enabled":false})");
        d.run(d.t0 + 2 * S);
        d.host.controls().invoke("nudge.plus", 1);
        CHECK_NEAR(d.host.latencyMs(), 1.0, 1e-9);
        CHECK_EQ(d.post("step", R"({"ms":1})").status, 409);
        d.feature.shutdown();
        CHECK(!d.host.controls().has("nudge.plus"));
    });
}

TEST_CASE("N-T6 confidence JSON equals the engine values on a replayed fixture") {
    WITH_TIMEOUT(20000, {
        Rig r(wrongBarCapture(2.0));
        for (int s = 3; s <= 12; s += 3) {
            r.run(r.t0 + s * S);
            const clock::PlayerTrack& tr = r.host.engine_.track(r.host.engine_.master());
            const Json st = r.feature.status();
            CHECK_NEAR(st.get("hitRate").asNumber(), tr.hitRate(), 1e-12);
            CHECK_NEAR(st.get("rmsMs").asNumber(), tr.rmsResidual() / 1e6, 1e-12);
            CHECK_NEAR(st.get("madMs").asNumber(), tr.mad() / 1e6, 1e-12);
            CHECK_JSON(st, "state", "locked");
            CHECK_JSON(st, "locked", true);
            CHECK_JSON(st, "formula", "0.5 hit + 0.3 jitter + 0.2 locked");
            CHECK_NEAR(st.get("confidence").asNumber(), r.host.engine_.timeline().confidence, 1e-6);
            // the three terms reproduce the confidence the engine published
            const double c = 0.5 * st.get("hitRate").asNumber() + 0.3 * st.get("jitterScore").asNumber() + 0.2 * 1.0;
            CHECK_NEAR(c, st.get("confidence").asNumber(), 1e-3);
            CHECK(st.get("hitsOf").asInt() <= 8);
            CHECK(st.get("hits").asInt() <= st.get("hitsOf").asInt());
            CHECK(!st.has("enabled"));
            CHECK(!st.has("error"));
        }
        const Json st = r.feature.status();
        CHECK_JSON(st, "tapReady", true);
        CHECK_JSON(st, "barOffset", 0);
        CHECK_NEAR(st.get("latencyMs").asNumber(), 1.0, 1e-9);
    });
}

TEST_CASE("N-T5b through Core and HTTP: routes, controls list, real latency and bar offset actions") {
    WITH_TIMEOUT(20000, {
        auto tmp = TEMP_DIR();
        Settings s;
        s.interfaceName = "fs06-no-such-interface";
        Core core(s, tmp.file("config.json"), tmp.path());
        core.start();
        auto req = [&](const std::string& m, const std::string& path, const std::string& body = "") {
            HttpRequest q; q.method = m; q.path = path; q.body = body;
            return handleRequest(core, q);
        };
        Json controls = parse(req("GET", "/api/controls").body);
        int found = 0;
        for (auto& c : controls.items()) {
            const std::string id = c.get("id").asString();
            found += id.rfind("nudge.", 0) == 0 || id.rfind("downbeat.", 0) == 0;
        }
        CHECK_EQ(found, 7);
        HttpResponse r = req("POST", "/api/f/nudge/tap");
        CHECK_EQ(r.status, 409);
        CHECK_JSON(parse(r.body), "error", "no master");
        CHECK_EQ(req("GET", "/api/f/nudge/tap").status, 405);
        CHECK_EQ(req("POST", "/api/f/nudge/undo").status, 409);
        r = req("POST", "/api/f/nudge/step", R"({"ms":0.5})");
        CHECK_EQ(r.status, 200);
        CHECK_EQ(req("POST", "/api/controls/nudge.plus", "{}").status, 200);
        // core latency moved by two 0.5 ms steps (first onTick may not have run: give it a moment)
        Json st;
        for (int i = 0; i < 200; ++i) {
            st = core.status();
            if (std::fabs(st.get("scope").get("latencyMs").asNumber() - 2.0) < 1e-6) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK_NEAR(st.get("scope").get("latencyMs").asNumber(), 2.0, 1e-6);
        const Json f = st.get("features").get("nudge");
        CHECK_JSON(f, "enabled", true);
        CHECK_JSON(f, "formula", "0.5 hit + 0.3 jitter + 0.2 locked");
        CHECK_JSON(f, "tapReady", false);
        Json sj = parse(req("GET", "/api/f/nudge/settings").body);
        CHECK_JSON(sj, "tapLatencyMs", 60);
        req("POST", "/api/config", R"({"features":{"nudge":{"tapLatencyMs":120,"haptics":false}}})");
        sj = parse(req("GET", "/api/f/nudge/settings").body);
        CHECK_JSON(sj, "tapLatencyMs", 120);
        CHECK_JSON(sj, "haptics", false);
        CHECK_JSON(sj, "wakeLock", true);
        core.stop();
    });
}
