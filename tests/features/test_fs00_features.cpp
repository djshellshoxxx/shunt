// FS-00 sections 1, 2 and 6 tests: feature registry, dummy feature lifecycle (init, settings,
// status, routes, session-cleared), exception containment, replay seam with dummy hooks, and the
// lock-order stress test (run under TSan in CI). Original Shunt code.
#include "TestFramework.h"
#include "shunt/app/Api.h"
#include "shunt/net/Packets.h"
#include <atomic>
#include <chrono>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

using namespace shunt::app;
namespace net = shunt::net;

namespace {
HttpRequest req(const std::string& method, const std::string& path, const std::string& body = "") {
    HttpRequest r;
    r.method = method;
    r.path = path;
    r.body = body;
    return r;
}
template <class F> bool waitFor(F cond, int ms = 3000) {
    for (int i = 0; i < ms / 5; ++i) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}
void pause(int ms) {   // a few short sleeps; the net loop runs every <= 50 ms without a stack
    for (int i = 0; i < ms / 5; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
}
Settings offlineSettings() {
    Settings s;
    s.interfaceName = "fs00-no-such-interface";   // no live stack, no sockets on 50000-50002
    return s;
}
std::string tmpDir(const char* tag) {
    static int n = 0;
    return "/tmp/shunt_fs00_" + std::string(tag) + "_" + std::to_string(::getpid()) + "_" + std::to_string(++n);
}
Json statusOf(Core& c, const std::string& id) { return c.status().get("features").get(id); }
std::string readAllFiles(const std::string& dir) {
    std::string all;
    if (DIR* d = ::opendir(dir.c_str())) {
        while (dirent* e = ::readdir(d)) {
            if (e->d_name[0] == '.') continue;
            std::ifstream f(dir + "/" + e->d_name);
            std::stringstream ss;
            ss << f.rdbuf();
            all += ss.str();
        }
        ::closedir(d);
    }
    return all;
}

// Records lifecycle calls. Own mutex taken only as a leaf (FS-00 locking rules).
struct Dummy : IFeature {
    explicit Dummy(std::string id, std::vector<std::string>* order = nullptr) : id_(std::move(id)), order_(order) {}
    std::string id() const override { return id_; }
    void init(FeatureHost& h, const Json& s) override {
        host = &h;
        std::lock_guard<std::mutex> lk(mu);
        initSettings = s;
        ++inits;
        if (order_) order_->push_back(id_);
    }
    void applySettings(const Json& s) override { std::lock_guard<std::mutex> lk(mu); applied = s; ++applies; }
    void onEvent(const net::Event&, int64_t) override { ++events; }
    void onTick(int64_t, int64_t) override { ++ticks; }
    Json status() const override { Json j = Json::object(); j["ticks"] = ticks.load(); j["kind"] = "dummy"; return j; }
    std::optional<HttpResponse> handle(const HttpRequest& r, const std::string& sub) override {
        if (sub == "echo") { HttpResponse x; x.body = "{\"sub\":\"echo\",\"method\":\"" + r.method + "\"}"; return x; }
        if (sub == "save") {
            Json v = Json::object();
            v["enabled"] = true;
            v["saved"] = 42;
            HttpResponse x;
            x.body = host->saveSettings(id_, v) ? "{\"ok\":true}" : "{\"ok\":false}";
            return x;
        }
        return std::nullopt;
    }
    void shutdown() override { ++shutdowns; }

    FeatureHost* host = nullptr;
    std::mutex mu;
    Json initSettings, applied;
    int applies = 0, inits = 0;
    std::atomic<int> ticks{0}, events{0}, shutdowns{0};

private:
    std::string id_;
    std::vector<std::string>* order_;
};

struct Thrower : IFeature {
    std::string id() const override { return "thrower"; }
    bool defaultEnabled() const override { return true; }
    void onTick(int64_t, int64_t) override { ++ticks; throw std::runtime_error("tick boom"); }
    std::optional<HttpResponse> handle(const HttpRequest&, const std::string& sub) override {
        ++handles;
        if (sub == "throw") throw std::runtime_error("handle boom");
        return HttpResponse{};
    }
    std::atomic<int> ticks{0}, handles{0};
};

// Replay hooks: feeds a CDJ that is master, playing, on air with track 4242, beats at 128 BPM.
struct Replayer : IFeature, ReplayHooks {
    std::string id() const override { return "replayer"; }
    bool defaultEnabled() const override { return true; }
    void init(FeatureHost& h, const Json&) override { host = &h; }
    void onEvent(const net::Event&, int64_t) override { ++events; }
    std::optional<HttpResponse> handle(const HttpRequest&, const std::string& sub) override {
        HttpResponse r;
        if (sub == "enter") r.body = host->enterReplay(this) ? "{\"ok\":true}" : "{\"ok\":false}";
        else if (sub == "leave") { host->leaveReplay(); r.body = "{\"ok\":true}"; }
        else return std::nullopt;
        return r;
    }
    void step(int64_t now) override {
        ++steps;
        if (throwInStep) throw std::runtime_error("step boom");
        const int64_t period = 468'750'000;   // 128 BPM
        uint8_t buf[512];
        if (nextBeatNs == 0) {                // first pass after enter: catch up 16 beats of history
            nextBeatNs = now - 16 * period;
            net::KeepAliveParams ka; ka.name = "CDJ-3000"; ka.number = 1; ka.ip = 0x0a000001;
            feedOk += host->feed(net::kAnnouncePort, buf, net::buildKeepAlive(buf, ka), nextBeatNs - 1000, 0x0a000001, 1700000000000) ? 1 : 0;
        }
        while (nextBeatNs <= now) {
            net::CdjStatusParams s;
            s.name = "CDJ-3000"; s.number = 1; s.rekordboxId = 4242; s.playState = 3; s.bpm100 = 12800;
            s.flags = net::kFlagPlay | net::kFlagMaster | net::kFlagOnAir;
            s.beatInBar = uint8_t(beat % 4 + 1);
            host->feed(net::kStatusPort, buf, net::buildCdjStatus(buf, s), nextBeatNs, 0x0a000001, 1700000000000 + beat * 468);
            net::BeatParams b;
            b.name = "CDJ-3000"; b.number = 1; b.bpm100 = 12800; b.beatInBar = uint8_t(beat % 4 + 1);
            b.nextBeatMs = 469; b.secondBeatMs = 938; b.nextBarMs = uint32_t((4 - beat % 4) * 469);
            if (host->feed(net::kBeatPort, buf, net::buildBeat(buf, b), nextBeatNs + 1000, 0x0a000001, 1700000000000 + beat * 468)) ++feedOk;
            nextBeatNs += period;
            ++beat;
        }
        if (wantOutputs) host->setOutputsEnabled(true);
        if (leaveInStep) host->leaveReplay();
    }
    FeatureHost* host = nullptr;
    std::atomic<int> steps{0}, events{0}, feedOk{0};
    std::atomic<bool> throwInStep{false}, leaveInStep{false}, wantOutputs{false};
    int64_t nextBeatNs = 0;
    int64_t beat = 0;
};
} // namespace

TEST_CASE("FS00-T4 feature registry: sorted creation, id rules") {
    FeatureRegistry reg;
    reg.add([] { return std::unique_ptr<IFeature>(new Dummy("zeta")); });
    reg.add([] { return std::unique_ptr<IFeature>(new Dummy("alpha")); });
    reg.add(nullptr);
    reg.add([] { return std::unique_ptr<IFeature>(); });
    CHECK_EQ(int(reg.size()), 3);
    auto all = reg.createAll();
    REQUIRE(all.size() == 2);
    CHECK(all[0]->id() == "alpha");
    CHECK(all[1]->id() == "zeta");
    CHECK(validFeatureId("onair"));
    CHECK(validFeatureId("fs00"));
    CHECK(!validFeatureId(""));
    CHECK(!validFeatureId("On-Air"));
    CHECK(!validFeatureId("a/b"));
    CHECK(!validFeatureId("../x"));
    CHECK(!validFeatureId(std::string(33, 'a')));
}

TEST_CASE("FS00-T5 dummy feature lifecycle: init order, settings, status, routes, session cleared") {
    const std::string dir = tmpDir("life");
    Settings s = offlineSettings();
    s.raw["features"]["dummy"]["x"] = 1;
    Core core(s, dir + "/config.json", dir);
    std::vector<std::string> order;
    auto* d = new Dummy("dummy", &order);
    CHECK(core.addFeature(std::unique_ptr<IFeature>(new Dummy("zeta", &order))));
    CHECK(core.addFeature(std::unique_ptr<IFeature>(d)));
    CHECK(core.addFeature(std::unique_ptr<IFeature>(new Dummy("alpha", &order))));
    CHECK(core.addFeature(std::unique_ptr<IFeature>(new Dummy("alpha", &order))));   // duplicate: dropped at start
    CHECK(core.addFeature(std::unique_ptr<IFeature>(new Dummy("Bad-Id", &order))));  // invalid: dropped at start
    CHECK(handleRequest(core, req("GET", "/api/features")).body == "[]");            // before start
    core.start();
    CHECK(!core.addFeature(std::unique_ptr<IFeature>(new Dummy("late"))));
    REQUIRE(order.size() == 3);
    CHECK(order[0] == "alpha" && order[1] == "dummy" && order[2] == "zeta");
    CHECK_EQ(d->inits, 1);
    CHECK_EQ(d->initSettings.get("x").asInt(), 1);

    HttpResponse r = handleRequest(core, req("GET", "/api/features"));
    CHECK_EQ(r.status, 200);
    CHECK(r.body == "[{\"id\":\"alpha\",\"enabled\":false,\"error\":\"\"},{\"id\":\"dummy\",\"enabled\":false,\"error\":\"\"},"
                    "{\"id\":\"zeta\",\"enabled\":false,\"error\":\"\"}]");
    Json st = statusOf(core, "dummy");
    CHECK(st.get("enabled").type() == Json::Bool && !st.get("enabled").asBool());
    CHECK(st.get("error").asString() == "");
    CHECK(st.get("kind").asString() == "dummy");
    pause(120);
    CHECK_EQ(d->ticks.load(), 0);      // disabled: inert

    r = handleRequest(core, req("POST", "/api/config", "{\"features\":{\"dummy\":{\"enabled\":true}}}"));
    CHECK_EQ(r.status, 200);
    {
        std::lock_guard<std::mutex> lk(d->mu);
        CHECK_EQ(d->applies, 1);
        CHECK(d->applied.get("enabled").asBool());
        CHECK_EQ(d->applied.get("x").asInt(), 1);          // deep merge kept x
    }
    CHECK(waitFor([&] { return d->ticks.load() > 2; }));
    CHECK(statusOf(core, "dummy").get("enabled").asBool());
    CHECK(statusOf(core, "dummy").get("ticks").asInt() > 0);

    handleRequest(core, req("POST", "/api/config", "{\"features\":{\"other\":{\"a\":1}}}"));
    { std::lock_guard<std::mutex> lk(d->mu); CHECK_EQ(d->applies, 1); }
    Json cfg = core.config();
    CHECK_EQ(cfg.get("features").get("dummy").get("x").asInt(), 1);
    CHECK(cfg.get("features").get("dummy").get("enabled").asBool());
    CHECK_EQ(cfg.get("features").get("other").get("a").asInt(), 1);

    r = handleRequest(core, req("GET", "/api/f/dummy/echo"));
    CHECK_EQ(r.status, 200);
    CHECK(r.body == "{\"sub\":\"echo\",\"method\":\"GET\"}");
    CHECK(handleRequest(core, req("POST", "/api/f/dummy/echo", "not json")).body.find("POST") != std::string::npos);
    CHECK(handleRequest(core, req("DELETE", "/api/f/dummy/echo")).body.find("DELETE") != std::string::npos);
    r = handleRequest(core, req("GET", "/api/f/dummy/nothing"));
    CHECK_EQ(r.status, 404);
    CHECK(r.body.find("\"error\"") != std::string::npos);
    CHECK_EQ(handleRequest(core, req("GET", "/api/f/dummy")).status, 404);
    CHECK_EQ(handleRequest(core, req("GET", "/api/f/missing/echo")).status, 404);
    CHECK_EQ(handleRequest(core, req("GET", "/api/f/Bad-Id/echo")).status, 404);
    CHECK_EQ(handleRequest(core, req("GET", "/api/f/../config")).status, 404);
    CHECK_EQ(handleRequest(core, req("PUT", "/api/config")).status, 405);   // other routes unchanged

    r = handleRequest(core, req("POST", "/api/f/dummy/save"));
    CHECK(r.body == "{\"ok\":true}");
    CHECK_EQ(d->host->settings("dummy").get("saved").asInt(), 42);
    CHECK(d->host->settings("nobody").isObject());
    Settings onDisk = Settings::load(dir + "/config.json");
    CHECK_EQ(onDisk.raw.get("features").get("dummy").get("saved").asInt(), 42);
    CHECK(onDisk.raw.get("features").get("other").has("a"));
    {
        CoreLockedScope locked;           // file I/O is refused inside Core-locked callbacks
        CHECK(!d->host->saveSettings("dummy", Json::object()));
    }

    int cleared = 0;
    d->host->onSessionCleared([&] { ++cleared; });
    CHECK_EQ(handleRequest(core, req("POST", "/api/session/clear", "{}")).status, 200);
    CHECK_EQ(cleared, 1);

    CHECK(d->host->wallNowMs() > 1600000000000LL);
    d->host->meta().set(77, shunt::log::TrackMeta{"T", "A", "", "", "8A", 120, 300});
    CHECK(d->host->meta().get(77).has_value());
    CHECK(d->host->meta().get(77)->key == "8A");
    CHECK(!d->host->meta().get(78).has_value());
    CHECK(d->host->controls().has("baroffset.cycle"));
    CHECK(!d->host->replaying());

    core.stop();
    CHECK_EQ(d->shutdowns.load(), 1);
}

TEST_CASE("FS00-T6 exceptions are contained; 5 in a minute put a feature in error state") {
    const std::string dir = tmpDir("throw");
    Core core(offlineSettings(), dir + "/config.json", dir);
    auto* t = new Thrower;
    auto* d = new Dummy("dummy");
    core.addFeature(std::unique_ptr<IFeature>(t));
    core.addFeature(std::unique_ptr<IFeature>(d));
    core.start();
    handleRequest(core, req("POST", "/api/config", "{\"features\":{\"dummy\":{\"enabled\":true}}}"));
    CHECK(waitFor([&] { return !statusOf(core, "thrower").get("error").asString("").empty(); }));
    pause(150);
    CHECK_EQ(t->ticks.load(), 5);                                   // no calls after the 5th exception
    CHECK(statusOf(core, "thrower").get("error").asString("").find("tick boom") != std::string::npos);
    CHECK(handleRequest(core, req("GET", "/api/features")).body.find("tick boom") != std::string::npos);
    CHECK_EQ(handleRequest(core, req("GET", "/api/f/thrower/ok")).status, 503);
    CHECK_EQ(t->handles.load(), 0);
    const int dt = d->ticks.load();
    CHECK(waitFor([&] { return d->ticks.load() > dt + 2; }));      // other features keep running
    // saving its settings again re-arms it
    handleRequest(core, req("POST", "/api/config", "{\"features\":{\"thrower\":{\"poke\":1}}}"));
    CHECK(waitFor([&] { return t->ticks.load() >= 10; }));
    CHECK(waitFor([&] { return !statusOf(core, "thrower").get("error").asString("").empty(); }));
    pause(100);
    CHECK_EQ(t->ticks.load(), 10);
    core.stop();

    // handle() exceptions: 500 each, then 503 after five
    const std::string dir2 = tmpDir("throw2");
    Core core2(offlineSettings(), dir2 + "/config.json", dir2);
    auto* t2 = new Thrower;
    core2.addFeature(std::unique_ptr<IFeature>(t2));
    // keep the ticking thrower disabled for this part
    handleRequest(core2, req("POST", "/api/config", "{\"features\":{\"thrower\":{\"enabled\":false}}}"));
    core2.start();
    CHECK_EQ(handleRequest(core2, req("GET", "/api/f/thrower/fine")).status, 200);
    for (int i = 0; i < 5; ++i) CHECK_EQ(handleRequest(core2, req("GET", "/api/f/thrower/throw")).status, 500);
    CHECK_EQ(handleRequest(core2, req("GET", "/api/f/thrower/fine")).status, 503);
    CHECK(statusOf(core2, "thrower").get("error").asString("").find("handle boom") != std::string::npos);
    core2.stop();
}

TEST_CASE("FS00-T7 replay seam: enter, step feeds the engine, leave restores live state") {
    const std::string dir = tmpDir("replay");
    Core core(offlineSettings(), dir + "/config.json", dir);
    auto* rp = new Replayer;
    core.addFeature(std::unique_ptr<IFeature>(rp));
    core.start();
    pause(60);
    const Json before = core.status();
    CHECK_EQ(before.get("events").asInt(), 0);
    {
        CoreLockedScope locked;
        CHECK(!rp->host->enterReplay(rp));     // refused under the Core lock
    }
    uint8_t buf[8] = {};
    CHECK(!rp->host->feed(net::kBeatPort, buf, sizeof buf, 0, 0));   // only inside step()

    CHECK(handleRequest(core, req("POST", "/api/f/replayer/enter")).body == "{\"ok\":true}");
    CHECK(rp->host->replaying());
    CHECK(handleRequest(core, req("POST", "/api/f/replayer/enter")).body == "{\"ok\":false}");   // already replaying
    CHECK(waitFor([&] { return rp->steps.load() > 3 && rp->feedOk.load() > 10; }));
    CHECK(waitFor([&] { return core.status().get("events").asInt() > 0; }));
    Json st = core.status();
    CHECK(st.get("claim").asString() == "replay");
    CHECK(rp->events.load() > 10);                                   // features see replayed events
    CHECK(waitFor([&] { Json s2 = core.status(); return s2.get("bpm").asNumber() > 127 && s2.get("bpm").asNumber() < 129; }));
    CHECK(waitFor([&] { return core.status().get("devices").size() >= 1; }));
    const double oscSent0 = core.status().get("outputs").get("osc").get("sent").asNumber();
    CHECK(core.tracklist().get("rows").size() + core.tracklist().get("events").size() > 0);   // replay session visible

    CHECK(handleRequest(core, req("POST", "/api/f/replayer/leave")).body == "{\"ok\":true}");
    CHECK(!rp->host->replaying());
    st = core.status();
    CHECK_EQ(st.get("events").asInt(), 0);                           // parked live session is back
    CHECK(st.get("claim").asString() == before.get("claim").asString());
    CHECK(waitFor([&] { return core.status().get("engineError").asString() == before.get("engineError").asString(); }));
    pause(60);
    CHECK(readAllFiles(dir + "/tracklists").find("4242") == std::string::npos);   // replay rows never persisted
    CHECK_EQ(oscSent0, 0.0);

    // leave from inside step() is deferred to the end of the pass
    rp->nextBeatNs = 0;
    rp->leaveInStep = true;
    CHECK(handleRequest(core, req("POST", "/api/f/replayer/enter")).body == "{\"ok\":true}");
    CHECK(waitFor([&] { return !rp->host->replaying(); }));
    rp->leaveInStep = false;

    // an exception in step() leaves the replay
    rp->nextBeatNs = 0;
    rp->throwInStep = true;
    CHECK(handleRequest(core, req("POST", "/api/f/replayer/enter")).body == "{\"ok\":true}");
    CHECK(waitFor([&] { return !rp->host->replaying(); }));
    rp->throwInStep = false;
    CHECK_EQ(core.status().get("events").asInt(), 0);

    // replay running at stop(): live session restored, nothing persisted
    rp->nextBeatNs = 0;
    CHECK(handleRequest(core, req("POST", "/api/f/replayer/enter")).body == "{\"ok\":true}");
    CHECK(waitFor([&] { return core.status().get("events").asInt() > 0; }));
    core.stop();
    CHECK(readAllFiles(dir + "/tracklists").find("4242") == std::string::npos);
}

TEST_CASE("FS00-T7b replay outputs are off by default and can be enabled by the hooks") {
    const std::string dir = tmpDir("replayout");
    Settings s = offlineSettings();
    s.outputs.osc.enabled = true;
    s.outputs.osc.host = "127.0.0.1";
    s.outputs.osc.port = 9;               // discard port; only the sent counter matters
    Core core(s, dir + "/config.json", dir);
    auto* rp = new Replayer;
    core.addFeature(std::unique_ptr<IFeature>(rp));
    core.start();
    CHECK(handleRequest(core, req("POST", "/api/f/replayer/enter")).body == "{\"ok\":true}");
    CHECK(waitFor([&] { return rp->feedOk.load() > 20; }));
    pause(100);
    CHECK_EQ(core.status().get("outputs").get("osc").get("sent").asNumber(), 0.0);
    rp->wantOutputs = true;
    CHECK(waitFor([&] { return core.status().get("outputs").get("osc").get("sent").asNumber() > 0; }));
    handleRequest(core, req("POST", "/api/f/replayer/leave"));
    core.stop();
}

namespace {
// Lock-order stress: own mutex inside Core-locked callbacks (allowed order Core -> own), and
// handle() calling the host and Core without holding its own mutex.
struct Stress : IFeature, ReplayHooks {
    std::string id() const override { return "stress"; }
    bool defaultEnabled() const override { return true; }
    void init(FeatureHost& h, const Json&) override {
        host = &h;
        ControlAction a;
        a.id = "stress.noop"; a.label = "noop"; a.group = "test";
        a.fn = [this](double) { std::lock_guard<std::mutex> lk(mu); ++invokes; };
        h.controls().add(a);
    }
    void applySettings(const Json& s) override { std::lock_guard<std::mutex> lk(mu); last = s; }
    void onEvent(const net::Event&, int64_t) override {
        std::lock_guard<std::mutex> lk(mu);
        ++evs;
        (void)host->meta().contains(uint32_t(evs));
        (void)host->settings("stress");
        (void)host->engine().state();
    }
    void onTick(int64_t, int64_t) override { std::lock_guard<std::mutex> lk(mu); ++ticks; }
    Json status() const override { std::lock_guard<std::mutex> lk(mu); Json j = Json::object(); j["evs"] = int64_t(evs); return j; }
    std::optional<HttpResponse> handle(const HttpRequest&, const std::string& sub) override {
        HttpResponse r;
        if (sub == "enter") { r.body = host->enterReplay(this) ? "{\"ok\":true}" : "{\"ok\":false}"; return r; }
        if (sub == "leave") { host->leaveReplay(); return r; }
        { std::lock_guard<std::mutex> lk(mu); ++handles; }      // released before calling out (rule 3)
        (void)host->settings("stress");
        host->meta().set(1, shunt::log::TrackMeta{});
        host->controls().invoke("stress.noop", 1);
        (void)host->controls().list();
        (void)host->replaying();
        return r;
    }
    void step(int64_t now) override {
        uint8_t buf[512];
        for (int i = 0; i < 20; ++i) {
            net::BeatParams b; b.number = uint8_t(1 + i % 2); b.bpm100 = 12800; b.nextBeatMs = 469;
            host->feed(net::kBeatPort, buf, net::buildBeat(buf, b), now - (20 - i) * 1000, 0x0a000001);
            net::CdjStatusParams s; s.number = uint8_t(1 + i % 2); s.rekordboxId = uint32_t(100 + i % 3); s.playState = 3;
            s.flags = uint8_t(net::kFlagPlay | (i % 2 ? 0 : net::kFlagMaster));
            host->feed(net::kStatusPort, buf, net::buildCdjStatus(buf, s), now - (20 - i) * 1000 + 500, 0x0a000001);
        }
    }
    FeatureHost* host = nullptr;
    mutable std::mutex mu;
    uint64_t evs = 0, ticks = 0, handles = 0, invokes = 0;
    Json last;
};
}

TEST_CASE("FS00-T8 lock order: handle() and HTTP routes against hammering onEvent for 2 s") {
    const std::string dir = tmpDir("stress");
    Core core(offlineSettings(), dir + "/config.json", dir);
    auto* sf = new Stress;
    core.addFeature(std::unique_ptr<IFeature>(sf));
    core.start();
    CHECK(handleRequest(core, req("POST", "/api/f/stress/enter")).body == "{\"ok\":true}");
    std::atomic<bool> stop{false};
    std::atomic<int> requests{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 3; ++t) {
        threads.emplace_back([&, t] {
            int i = 0;
            while (!stop) {
                switch ((i + t) % 6) {
                case 0: handleRequest(core, req("GET", "/api/f/stress/x")); break;
                case 1: handleRequest(core, req("GET", "/api/status")); break;
                case 2: handleRequest(core, req("POST", "/api/controls/stress.noop", "{}")); break;
                case 3: handleRequest(core, req("GET", "/api/features")); break;
                case 4: if (i % 30 == 4) handleRequest(core, req("POST", "/api/config", "{\"features\":{\"stress\":{\"n\":" + std::to_string(i) + "}}}")); break;
                default: handleRequest(core, req("GET", "/api/tracklist")); break;
                }
                ++i;
                ++requests;
            }
        });
    }
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    stop = true;
    for (auto& th : threads) th.join();
    handleRequest(core, req("POST", "/api/f/stress/leave"));
    core.stop();
    std::lock_guard<std::mutex> lk(sf->mu);
    INFO("stress: %llu events, %llu ticks, %llu handles, %llu invokes, %d requests\n", (unsigned long long)sf->evs,
         (unsigned long long)sf->ticks, (unsigned long long)sf->handles, (unsigned long long)sf->invokes, requests.load());
    CHECK(sf->evs > 1000);
    CHECK(sf->ticks > 10);
    CHECK(sf->handles > 10);
    CHECK(sf->invokes > 10);
    CHECK(sf->last.has("n"));
}
