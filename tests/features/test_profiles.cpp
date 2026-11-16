// FS-07 tests PR-T1..PR-T8 (docs/specs/FS-07-venue-profiles.md): save/apply round trip, diff and the
// restart-confirmation rule, whitelist, import validation, slug rules and traversal, dirty flag, atomic
// write, per-profile registry actions; plus footswitch safety, limits and a Core integration run with the
// registered feature (features-only backend). Original Shunt code.
#include "TestFramework.h"
#include "../../app/src/features/Profiles.h"
#include "shunt/app/Api.h"
#include "shunt/app/Core.h"
#include "shunt/app/Settings.h"
#include <cstdlib>
#include <fstream>
#include <set>
#include <thread>
#include <chrono>
#include <sstream>
#include <sys/stat.h>

using namespace shunt;
using namespace shunt::app;
using namespace shunt::app::profiles;

namespace {
Json parseJson(const std::string& t) { Json j; Json::parse(t, j); return j; }

// Stand-in for Core: a Settings object with Core::applyConfig semantics (deep merge + clamping).
struct FakeBackend : ProfileBackend {
    Settings s;
    std::mutex mu;
    int applies = 0;
    Json config() override { std::lock_guard<std::mutex> lk(mu); return s.toJson(); }
    bool applyConfig(const Json& patch, std::string&) override {
        std::lock_guard<std::mutex> lk(mu);
        s = Settings::fromJson(patch, s);
        ++applies;
        return true;
    }
    std::string configDir() override { return ""; }
    void set(const std::string& json) { std::lock_guard<std::mutex> lk(mu); s = Settings::fromJson(parseJson(json), s); }
};

struct FakeHost : FeatureHost {
    explicit FakeHost(FakeBackend& b) : b_(b), outputs_([] { return clock::Timeline{}; }) {}
    int64_t wallNowMs() const override { return 1'760'000'000'000; }
    MetaStore& meta() override { return meta_; }
    ControlRegistry& controls() override { return controls_; }
    out::OutputManager& outputs() override { return outputs_; }
    Json settings(const std::string& id) override { Json f = b_.config().get("features").get(id); return f.isObject() ? f : Json::object(); }
    bool saveSettings(const std::string&, const Json&) override { return false; }
    const clock::ClockEngine& engine() const override { return engine_; }
    void onSessionCleared(std::function<void()>) override {}
    bool enterReplay(ReplayHooks*) override { return false; }
    void leaveReplay() override {}
    bool replaying() const override { return false; }
    bool feed(uint16_t, const uint8_t*, size_t, int64_t, uint32_t, int64_t) override { return false; }
    void resetEngine(bool) override {}
    void setOutputsEnabled(bool) override {}
    FakeBackend& b_;
    MetaStore meta_;
    ControlRegistry controls_;
    clock::ClockEngine engine_;
    out::OutputManager outputs_;
};

struct Rig {
    explicit Rig(const std::string& cfg = "{}") : be(std::make_shared<FakeBackend>()), host(*be), dir(TEMP_DIR()) {
        be->set("{\"mode\":\"follow\",\"deviceNumber\":5,\"latencyMs\":1.0,\"interface\":\"eth0\",\"http\":{\"port\":8080,\"bind\":\"0.0.0.0\"},\"features\":{\"onair\":{\"enabled\":true,\"holdMs\":4000},\"profiles\":{\"enabled\":true}}}");
        if (cfg != "{}") be->set(cfg);
        ProfilesOptions o;
        o.backend = be;
        o.dir = dir.file("profiles");
        o.pollMs = 0;
        o.beforeRename = [this] { return !failRename; };
        f = std::make_unique<ProfilesFeature>(o);
        Json en = Json::object();
        en["enabled"] = true;
        f->init(host, en);
    }
    HttpResponse call(const std::string& method, const std::string& sub, const std::string& body = "", const std::string& query = "") {
        HttpRequest r;
        r.method = method;
        r.path = "/api/f/profiles/" + sub;
        r.query = query;
        r.body = body;
        auto resp = f->handle(r, sub);
        REQUIRE(resp.has_value());
        return *resp;
    }
    Json post(const std::string& sub, const std::string& body, int* status = nullptr) {
        auto r = call("POST", sub, body);
        if (status) *status = r.status;
        return parseJson(r.body);
    }
    std::shared_ptr<FakeBackend> be;
    FakeHost host;
    tf::TempDir dir;
    std::unique_ptr<ProfilesFeature> f;
    bool failRename = false;
};
} // namespace

namespace {
std::string profileDoc(const std::string& settings, const std::string& name = "Imported", const std::string& extra = "") {
    return "{\"schema\":1,\"name\":\"" + name + "\"," + extra + "\"settings\":" + settings + "}";
}
bool exists(const std::string& p) { struct stat st; return ::stat(p.c_str(), &st) == 0; }
std::string slurp(const std::string& p) { std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
}

TEST_CASE("PR-T1 save then apply round trip on a modified settings object") {
    Rig r;
    r.be->set("{\"latencyMs\":1.5,\"venue\":\"Club A\",\"beatOnly\":true,\"midi\":{\"enabled\":true,\"port\":\"/dev/snd/midiC1D0\"},"
              "\"osc\":{\"enabled\":true,\"host\":\"10.0.0.9\",\"port\":9100,\"profile\":\"qlab\",\"qlabMasterCue\":7},\"features\":{\"onair\":{\"holdMs\":2500}}}");
    int st = 0;
    Json j = r.post("save", "{\"name\":\"Club A (wired)\"}", &st);
    CHECK(st == 200);
    CHECK_JSON(j, "slug", "club-a-wired");
    CHECK(exists(r.dir.file("profiles/club-a-wired.json")));
    // change everything, then apply
    r.be->set("{\"latencyMs\":-3,\"venue\":\"Other\",\"beatOnly\":false,\"midi\":{\"enabled\":false,\"port\":\"\"},"
              "\"osc\":{\"enabled\":false,\"host\":\"127.0.0.1\",\"port\":9000,\"profile\":\"generic\",\"qlabMasterCue\":1},\"features\":{\"onair\":{\"holdMs\":9000}}}");
    j = r.post("apply", "{\"slug\":\"club-a-wired\"}", &st);
    CHECK(st == 200);
    CHECK_JSON(j, "ok", true);
    CHECK_JSON(j, "restart", false);
    Json c = r.be->config();
    CHECK_NEAR(c.get("latencyMs").asNumber(), 1.5, 1e-9);
    CHECK_STR(c.get("venue").asString(), "Club A");
    CHECK(c.get("beatOnly").asBool());
    CHECK_STR(c.get("midi").get("port").asString(), "/dev/snd/midiC1D0");
    CHECK_STR(c.get("osc").get("host").asString(), "10.0.0.9");
    CHECK_STR(c.get("osc").get("profile").asString(), "qlab");
    CHECK(c.get("osc").get("qlabMasterCue").asInt() == 7);
    CHECK(c.get("features").get("onair").get("holdMs").asInt() == 2500);
    CHECK(c.get("features").get("onair").get("enabled").asBool());    // other features keys survive the deep merge
    CHECK_STR(c.get("features").get("profiles").get("active").asString(), "club-a-wired");
    // applying again changes nothing
    j = r.post("diff", "{\"slug\":\"club-a-wired\"}");
    CHECK(j.get("changes").size() == 0);
}

TEST_CASE("PR-T2 diff correctness and the restart-confirmation rule") {
    Rig r;
    r.post("save", "{\"name\":\"Base\",\"includeInterface\":true}");
    r.be->set("{\"latencyMs\":4.5,\"mode\":\"passive\",\"osc\":{\"port\":9555}}");
    Json d = r.post("diff", "{\"slug\":\"base\"}");
    CHECK(d.get("restart").asBool());
    CHECK(d.get("changes").size() == 3);
    std::set<std::string> paths;
    for (auto& ch : d.get("changes").items()) paths.insert(ch.get("path").asString());
    CHECK(paths.count("latencyMs") && paths.count("mode") && paths.count("osc.port"));
    for (auto& ch : d.get("changes").items())
        if (ch.get("path").asString() == "latencyMs") { CHECK_NEAR(ch.get("current").asNumber(), 4.5, 1e-9); CHECK_NEAR(ch.get("profile").asNumber(), 1.0, 1e-9); }
    int st = 0;
    Json a = r.post("apply", "{\"slug\":\"base\"}", &st);
    CHECK(st == 409);                                   // mode differs: network restart needs confirm
    CHECK(a.get("changes").size() == 3);
    CHECK_STR(r.be->config().get("mode").asString(), "passive");        // nothing applied
    a = r.post("apply", "{\"slug\":\"base\",\"confirm\":true}", &st);
    CHECK(st == 200);
    CHECK(a.get("restart").asBool());
    CHECK_STR(r.be->config().get("mode").asString(), "follow");
    // interface differs but includeInterface is off: not part of the diff
    r.post("save", "{\"name\":\"NoIf\"}");
    r.be->set("{\"interface\":\"wlan0\"}");
    d = r.post("diff", "{\"slug\":\"noif\"}");
    CHECK(d.get("changes").size() == 0);
    CHECK(!d.get("restart").asBool());
    // deviceNumber alone also needs confirmation; bad confirm type is rejected
    r.post("save", "{\"name\":\"Dev\",\"overwrite\":true}");
    r.be->set("{\"deviceNumber\":3}");
    r.post("apply", "{\"slug\":\"dev\",\"confirm\":\"yes\"}", &st);
    CHECK(st == 400);
    r.post("apply", "{\"slug\":\"dev\"}", &st);
    CHECK(st == 409);
    r.post("apply", "{\"slug\":\"nope\"}", &st);
    CHECK(st == 404);
}

TEST_CASE("PR-T3 whitelist: bar-offset memory and http bind untouched") {
    Rig r;
    r.be->set("{\"barOffset\":2,\"http\":{\"port\":9999,\"bind\":\"127.0.0.1\"},\"secretThing\":\"x\",\"features\":{\"onair\":{\"holdMs\":1111}}}");
    r.post("save", "{\"name\":\"W\"}");
    const Json file = parseJson(slurp(r.dir.file("profiles/w.json")));
    const Json& s = file.get("settings");
    CHECK(!s.has("barOffset") && !s.has("http") && !s.has("secretThing"));
    CHECK(!s.get("features").has("profiles"));            // the feature's own state (active) is not in a profile
    CHECK(s.get("features").has("onair"));
    // a hand-edited file carrying forbidden keys: dropped on apply, never applied
    std::ofstream(r.dir.file("profiles/evil.json")) << profileDoc("{\"barOffset\":3,\"http\":{\"port\":1,\"bind\":\"1.2.3.4\"},\"latencyMs\":2.5}", "Evil");
    r.be->set("{\"barOffset\":1,\"http\":{\"port\":8123,\"bind\":\"0.0.0.0\"}}");
    int st = 0;
    r.post("apply", "{\"slug\":\"evil\"}", &st);
    CHECK(st == 200);
    Json c = r.be->config();
    CHECK(c.get("barOffset").asInt() == 1);
    CHECK(c.get("http").get("port").asInt() == 8123);
    CHECK_STR(c.get("http").get("bind").asString(), "0.0.0.0");
    CHECK_NEAR(c.get("latencyMs").asNumber(), 2.5, 1e-9);
}

TEST_CASE("PR-T4 import: wrong schema, wrong types, oversize, unknown keys") {
    Rig r;
    int st = 0;
    Json j = r.post("import", "{\"schema\":2,\"name\":\"x\",\"settings\":{}}", &st);
    CHECK(st == 422); CHECK(j.get("error").asString().find("schema") != std::string::npos);
    j = r.post("import", "{\"name\":\"x\",\"settings\":{}}", &st);
    CHECK(st == 422);
    j = r.post("import", profileDoc("{\"latencyMs\":\"fast\"}"), &st);
    CHECK(st == 422); CHECK(j.get("error").asString().find("settings.latencyMs") != std::string::npos);
    j = r.post("import", profileDoc("{\"midi\":{\"port\":5}}"), &st);
    CHECK(st == 422); CHECK(j.get("error").asString().find("settings.midi.port") != std::string::npos);
    j = r.post("import", profileDoc("{\"features\":{\"onair\":3}}"), &st);
    CHECK(st == 422); CHECK(j.get("error").asString().find("settings.features.onair") != std::string::npos);
    j = r.post("import", profileDoc("[]"), &st);
    CHECK(st == 422);
    j = r.post("import", "{\"schema\":1,\"name\":7,\"settings\":{}}", &st);
    CHECK(st == 422);
    j = r.post("import", "{\"schema\":1,\"name\":\"a\\nb\",\"settings\":{}}", &st);
    CHECK(st == 422);
    j = r.post("import", "[1,2]", &st);
    CHECK(st == 400);
    std::string big = profileDoc("{\"venue\":\"x\"}", "Big");
    big.insert(big.size() - 1, ",\"pad\":\"" + std::string(70000, 'a') + "\"");
    j = r.post("import", big, &st);
    CHECK(st == 413);
    // unknown keys -> warnings, clamping -> warnings, file still imported
    j = r.post("import", profileDoc("{\"latencyMs\":500,\"deviceNumber\":9,\"zzz\":1,\"osc\":{\"bogus\":true,\"port\":9001},\"http\":{\"port\":1}}", "Good", "\"flavour\":\"x\","), &st);
    CHECK(st == 200);
    CHECK_JSON(j, "slug", "good");
    std::string w = j.get("warnings").dump();
    CHECK(w.find("settings.zzz") != std::string::npos);
    CHECK(w.find("settings.osc.bogus") != std::string::npos);
    CHECK(w.find("settings.http") != std::string::npos);
    CHECK(w.find("flavour") != std::string::npos);
    CHECK(w.find("settings.latencyMs clamped") != std::string::npos);
    const Json file = parseJson(slurp(r.dir.file("profiles/good.json")));
    CHECK_NEAR(file.get("settings").get("latencyMs").asNumber(), 50, 1e-9);
    CHECK(file.get("settings").get("deviceNumber").asInt() == 6);
    CHECK(file.get("settings").get("osc").get("port").asInt() == 9001);
    // import never overwrites silently
    j = r.post("import", profileDoc("{}", "Good"), &st);
    CHECK_JSON(j, "slug", "good-2");
    CHECK(exists(r.dir.file("profiles/good.json")) && exists(r.dir.file("profiles/good-2.json")));
    // export round trips through import
    auto ex = r.call("GET", "export", "", "slug=good");
    CHECK(ex.status == 200);
    CHECK(!ex.headers.empty());
    j = r.post("import", ex.body, &st);
    CHECK(st == 200); CHECK_JSON(j, "slug", "good-3");
}

TEST_CASE("PR-T5 slug rules, collision suffix, traversal attempts") {
    CHECK_STR(slugify("Club A (wired)"), "club-a-wired");
    CHECK_STR(slugify("  --Hello   World--  "), "hello-world");
    CHECK_STR(slugify("../../etc/passwd"), "etc-passwd");
    CHECK_STR(slugify("/abs/path"), "abs-path");
    CHECK_STR(slugify(std::string("a\0b", 3)), "a-b");
    CHECK_STR(slugify("\xD0\x9A\xD0\xBB\xD1\x83\xD0\xB1 A"), "a");            // Cyrillic confusables become '-'
    CHECK_STR(slugify("\xD0\xB0pple"), "pple");                                // Cyrillic a, not Latin
    CHECK_STR(slugify("???"), "profile");
    CHECK(slugify(std::string(200, 'x')).size() == 40);
    CHECK(validSlug("a-1") && !validSlug("") && !validSlug("A") && !validSlug("a/b") && !validSlug("..") && !validSlug(std::string(41, 'a')));
    CHECK(!validSlug(std::string("a\0b", 3)) && !validSlug("a.json") && !validSlug("-a-b") == false);
    std::set<std::string> taken = {"x", "x-2"};
    auto isTaken = [&](const std::string& s) { return taken.count(s) > 0; };
    const std::string x3 = uniqueSlug("x", isTaken);
    CHECK_STR(x3, "x-3");
    taken = {std::string(40, 'q')};
    const std::string u = uniqueSlug(std::string(40, 'q'), isTaken);
    CHECK(u.size() == 40 && u.substr(38) == "-2");

    Rig r;
    int st = 0;
    r.post("save", "{\"name\":\"Club-A\"}", &st);
    Json j = r.post("save", "{\"name\":\"Club A\"}", &st);               // same slug, different name -> -2
    CHECK(st == 200); CHECK_JSON(j, "slug", "club-a-2");
    j = r.post("save", "{\"name\":\"Club A\"}", &st);                    // same name: needs overwrite
    CHECK(st == 409); CHECK_JSON(j, "exists", true);
    j = r.post("save", "{\"name\":\"Club A\",\"overwrite\":true}", &st);
    CHECK(st == 200); CHECK_JSON(j, "slug", "club-a-2");
    // every path-ish slug is refused before touching the disk
    for (const char* bad : {"../x", "..", "/etc/passwd", "a/b", "A", "", "x.json", "a b"}) {
        for (const char* op : {"apply", "diff", "delete", "rename", "duplicate"}) {
            r.post(op, std::string("{\"slug\":\"") + bad + "\",\"name\":\"n\"}", &st);
            CHECK(st == 400);
        }
        auto ex = r.call("GET", "export", "", std::string("slug=") + bad);
        CHECK(ex.status == 400);
    }
    r.post("apply", "{\"slug\":\"a\\u0000b\"}", &st);
    CHECK(st == 400);
    r.post("save", "{\"name\":\"x\",\"slug\":\"../escape\",\"overwrite\":true}", &st);
    CHECK(st == 400);
    CHECK(!exists(r.dir.file("escape.json")));
    // names with traversal characters are harmless: only the slug becomes a path
    j = r.post("save", "{\"name\":\"../../evil\"}", &st);
    CHECK(st == 200); CHECK_JSON(j, "slug", "evil");
    CHECK(exists(r.dir.file("profiles/evil.json")));
}

TEST_CASE("PR-T6 dirty flag toggles with a setting change") {
    Rig r;
    Json l = parseJson(r.call("GET", "list").body);
    CHECK_JSON(l, "dirty", false);
    CHECK_JSON(l, "active", "");
    r.post("save", "{\"name\":\"Rehearsal room\"}");
    l = parseJson(r.call("GET", "list").body);
    CHECK_JSON(l, "active", "rehearsal-room");
    CHECK_JSON(l, "activeName", "Rehearsal room");
    CHECK_JSON(l, "dirty", false);
    CHECK(r.f->status().get("dirty").asBool() == false);
    r.be->set("{\"latencyMs\":7}");
    r.f->refreshNow();
    CHECK(r.f->status().get("dirty").asBool());
    CHECK_STR(r.f->status().get("active").asString(), "rehearsal-room");
    CHECK(!r.f->status().has("enabled") && !r.f->status().has("error"));
    r.be->set("{\"latencyMs\":1}");
    r.f->refreshNow();
    CHECK(!r.f->status().get("dirty").asBool());
    // a change in a non-profile setting does not make it dirty
    r.be->set("{\"barOffset\":2}");
    r.f->refreshNow();
    CHECK(!r.f->status().get("dirty").asBool());
    // a feature setting does
    r.be->set("{\"features\":{\"onair\":{\"holdMs\":1}}}");
    r.f->refreshNow();
    CHECK(r.f->status().get("dirty").asBool());
    // deleting the active profile clears active
    r.post("delete", "{\"slug\":\"rehearsal-room\"}");
    CHECK_STR(r.f->status().get("active").asString(), "");
    CHECK(!r.f->status().get("dirty").asBool());
}

TEST_CASE("PR-T7 atomic write: a failure between temp file and rename leaves the old file valid") {
    Rig r;
    r.be->set("{\"venue\":\"First\"}");
    int st = 0;
    r.post("save", "{\"name\":\"P\"}", &st);
    CHECK(st == 200);
    const std::string before = slurp(r.dir.file("profiles/p.json"));
    r.be->set("{\"venue\":\"Second\"}");
    r.failRename = true;                                   // "killed" after the temp file is complete
    r.post("save", "{\"name\":\"P\",\"overwrite\":true}", &st);
    CHECK(st == 500);
    CHECK(slurp(r.dir.file("profiles/p.json")) == before);
    CHECK(exists(r.dir.file("profiles/.p.json.tmp")));    // the stray temp is left behind ...
    Json l = parseJson(r.call("GET", "list").body);       // ... and ignored
    CHECK(l.get("profiles").size() == 1);
    r.failRename = false;
    r.post("save", "{\"name\":\"P\",\"overwrite\":true}", &st);
    CHECK(st == 200);
    CHECK(slurp(r.dir.file("profiles/p.json")) != before);
    CHECK(!exists(r.dir.file("profiles/.p.json.tmp")));
    // a truncated file on disk is skipped by list, and refused by apply with 422
    std::ofstream(r.dir.file("profiles/broken.json")) << "{\"schema\":1,\"name\":\"Br";
    l = parseJson(r.call("GET", "list").body);
    CHECK(l.get("profiles").size() == 1);
    r.post("apply", "{\"slug\":\"broken\"}", &st);
    CHECK(st == 422);
}

TEST_CASE("PR-T8 registry actions per profile appear and disappear with the list") {
    Rig r;
    auto has = [&](const std::string& id) { return r.host.controls().has(id); };
    CHECK(has("profile.next") && has("profile.prev"));
    CHECK(!has("profile.apply:a"));
    r.post("save", "{\"name\":\"A\"}");
    r.post("save", "{\"name\":\"B\"}");
    CHECK(has("profile.apply:a") && has("profile.apply:b"));
    // rename keeps the slug and updates the label
    r.post("rename", "{\"slug\":\"a\",\"name\":\"Alpha\"}");
    bool label = false;
    for (auto& c : r.host.controls().list()) if (c.id == "profile.apply:a" && c.label == "Profile: Alpha") label = true;
    CHECK(label);
    // a file copied in by hand appears on the next list
    std::ofstream(r.dir.file("profiles/c.json")) << profileDoc("{\"venue\":\"C\"}", "C", "\"slug\":\"c\",");
    r.call("GET", "list");
    CHECK(has("profile.apply:c"));
    // invoke applies, next/prev cycle alphabetically
    r.be->set("{\"venue\":\"zzz\"}");
    CHECK(r.host.controls().invoke("profile.apply:c", 1));
    CHECK_STR(r.be->config().get("venue").asString(), "C");
    CHECK_STR(r.f->status().get("active").asString(), "c");
    CHECK(r.host.controls().invoke("profile.next", 1));          // c -> a (wraps)
    CHECK_STR(r.f->status().get("active").asString(), "a");
    CHECK(r.host.controls().invoke("profile.next", 1));          // a (Alpha) -> b
    CHECK_STR(r.f->status().get("active").asString(), "b");
    CHECK(r.host.controls().invoke("profile.prev", 1));
    CHECK_STR(r.f->status().get("active").asString(), "a");
    r.post("delete", "{\"slug\":\"a\"}");
    CHECK(!has("profile.apply:a"));
    CHECK(has("profile.apply:b"));
    r.post("duplicate", "{\"slug\":\"b\"}");
    CHECK(has("profile.apply:b-copy"));
    // disabling the feature removes every action
    Json off = Json::object();
    off["enabled"] = false;
    r.f->applySettings(off);
    CHECK(!has("profile.next") && !has("profile.apply:b"));
    Json on = Json::object();
    on["enabled"] = true;
    r.f->applySettings(on);
    CHECK(has("profile.next") && has("profile.apply:b"));
    r.f->shutdown();
    CHECK(!has("profile.next") && !has("profile.apply:b"));
}

TEST_CASE("PR-T9 a footswitch never restarts the network") {
    Rig r;
    r.post("save", "{\"name\":\"Stage\"}");
    r.be->set("{\"mode\":\"passive\",\"latencyMs\":9}");
    CHECK(r.host.controls().invoke("profile.apply:stage", 1));
    CHECK_STR(r.be->config().get("mode").asString(), "passive");   // refused
    CHECK_NEAR(r.be->config().get("latencyMs").asNumber(), 9, 1e-9);
    CHECK(r.f->status().get("warning").asString().find("Profiles page") != std::string::npos);
    r.be->set("{\"mode\":\"follow\"}");
    CHECK(r.host.controls().invoke("profile.apply:stage", 1));
    CHECK_NEAR(r.be->config().get("latencyMs").asNumber(), 1.0, 1e-9);
    CHECK_STR(r.f->status().get("warning").asString(), "");
}

TEST_CASE("PR-T10 limits and validation: 100 profiles, body size, methods, types") {
    Rig r;
    int st = 0;
    for (int i = 0; i < 100; ++i) { r.post("save", "{\"name\":\"p" + std::to_string(i) + "\"}", &st); REQUIRE(st == 200); }
    r.post("save", "{\"name\":\"one too many\"}", &st);
    CHECK(st == 409);
    r.post("import", profileDoc("{}", "Extra"), &st);
    CHECK(st == 409);
    r.post("duplicate", "{\"slug\":\"p1\"}", &st);
    CHECK(st == 409);
    CHECK(parseJson(r.call("GET", "list").body).get("profiles").size() == 100);
    r.post("save", "{\"name\":5}", &st); CHECK(st == 400);
    r.post("save", "{\"name\":\"\"}", &st); CHECK(st == 400);
    r.post("save", "{\"name\":\"" + std::string(81, 'n') + "\"}", &st); CHECK(st == 400);
    r.post("save", "{\"name\":\"ok\",\"includeInterface\":1}", &st); CHECK(st == 400);
    r.post("save", std::string(70000, ' '), &st); CHECK(st == 413);
    CHECK(r.call("GET", "save").status == 405);
    CHECK(r.call("POST", "list").status == 405);
    CHECK(!r.f->handle([] { HttpRequest q; q.method = "GET"; return q; }(), "nonsense").has_value());
}

TEST_CASE("PR-T11 duplicate, rename, includeInterface apply, Core integration with the registered feature") {
    Rig r;
    int st = 0;
    r.post("save", "{\"name\":\"Net\",\"includeInterface\":true}");
    r.be->set("{\"interface\":\"wlan9\"}");
    Json d = r.post("diff", "{\"slug\":\"net\"}");
    CHECK(d.get("restart").asBool());
    CHECK_STR(d.get("changes").items()[0].get("path").asString(), "interface");
    r.post("apply", "{\"slug\":\"net\"}", &st);
    CHECK(st == 409);
    r.post("apply", "{\"slug\":\"net\",\"confirm\":true}", &st);
    CHECK(st == 200);
    CHECK_STR(r.be->config().get("interface").asString(), "eth0");
    Json j = r.post("duplicate", "{\"slug\":\"net\"}", &st);
    CHECK_JSON(j, "slug", "net-copy");
    j = r.post("rename", "{\"slug\":\"net-copy\",\"name\":\"Renamed\"}", &st);
    CHECK(st == 200);
    Json l = parseJson(r.call("GET", "list").body);
    CHECK_JSON(l, "profiles[0].name", "Net");          // sorted by name: Net, Renamed
    CHECK_JSON(l, "profiles[1].name", "Renamed");
    CHECK_JSON(l, "profiles[1].slug", "net-copy");
    CHECK_JSON(l, "profiles[1].includeInterface", true);
    r.post("delete", "{\"slug\":\"nothere\"}", &st);
    CHECK(st == 404);

    // Core: the registered feature uses the real Core settings (FeatureHost whole-config seam)
    // and keeps profiles next to config.json
    auto dir = TEMP_DIR();
    const char* oldXdg = std::getenv("XDG_CONFIG_HOME");
    const std::string saved = oldXdg ? oldXdg : "";
    ::setenv("XDG_CONFIG_HOME", dir.path().c_str(), 1);
    {
        Settings s;
        s.interfaceName = "fs07-no-such-interface";
        Core core(s, dir.file("config.json"), dir.path());
        core.start();
        auto req = [](const std::string& m, const std::string& p, const std::string& b = "") { HttpRequest q; q.method = m; q.path = p; q.body = b; return q; };
        handleRequest(core, req("POST", "/api/config", "{\"features\":{\"profiles\":{\"enabled\":true},\"zeta\":{\"a\":1}}}"));
        Json sv = parseJson(handleRequest(core, req("POST", "/api/f/profiles/save", "{\"name\":\"Core test\"}")).body);
        CHECK_JSON(sv, "slug", "core-test");
        CHECK(exists(dir.file("profiles/core-test.json")));
        Json ls = parseJson(handleRequest(core, req("GET", "/api/f/profiles/list")).body);
        CHECK_JSON(ls, "backend", "core");
        CHECK_JSON(ls, "profiles[0].slug", "core-test");
        Json ctl = parseJson(handleRequest(core, req("GET", "/api/controls")).body);
        bool found = false;
        for (auto& c : ctl.items()) found = found || c.get("id").asString() == "profile.apply:core-test";
        CHECK(found);
        Json stt = parseJson(handleRequest(core, req("GET", "/api/status")).body);
        CHECK_JSON(stt, "features.profiles.count", 1);
        CHECK_JSON(stt, "features.profiles.active", "core-test");
        Json ap = parseJson(handleRequest(core, req("POST", "/api/f/profiles/apply", "{\"slug\":\"core-test\"}")).body);
        CHECK_JSON(ap, "ok", true);
        // A non-feature setting changed after saving is restored by apply through Core.
        const double savedLatency = core.config().get("latencyMs").asNumber(0);
        handleRequest(core, req("POST", "/api/config", "{\"latencyMs\":17.5}"));
        CHECK_NEAR(core.config().get("latencyMs").asNumber(0), 17.5, 1e-9);
        Json dirty = parseJson(handleRequest(core, req("GET", "/api/f/profiles/list")).body);
        (void)dirty;
        ap = parseJson(handleRequest(core, req("POST", "/api/f/profiles/apply", "{\"slug\":\"core-test\"}")).body);
        CHECK_JSON(ap, "ok", true);
        CHECK_NEAR(core.config().get("latencyMs").asNumber(0), savedLatency, 1e-9);
        core.stop();
    }
    if (oldXdg) ::setenv("XDG_CONFIG_HOME", saved.c_str(), 1); else ::unsetenv("XDG_CONFIG_HOME");
}

TEST_CASE("PR-T12 background refresh thread keeps dirty current (TSan)") {
    auto be = std::make_shared<FakeBackend>();
    FakeHost host(*be);
    auto dir = TEMP_DIR();
    ProfilesOptions o;
    o.backend = be;
    o.dir = dir.file("profiles");
    o.pollMs = 2;
    ProfilesFeature f(o);
    Json en = Json::object();
    en["enabled"] = true;
    be->set("{\"features\":{\"profiles\":{\"enabled\":true}}}");
    f.init(host, en);
    HttpRequest q;
    q.method = "POST";
    q.body = "{\"name\":\"T\"}";
    REQUIRE(f.handle(q, "save").value().status == 200);
    WITH_TIMEOUT(10000, {
        be->set("{\"latencyMs\":12}");
        bool dirty = false;
        for (int i = 0; i < 2000 && !dirty; ++i) {
            dirty = f.status().get("dirty").asBool();
            if (!dirty) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(dirty);
        std::thread t([&] { for (int i = 0; i < 50; ++i) { HttpRequest g; g.method = "GET"; f.handle(g, "list"); } });
        for (int i = 0; i < 50; ++i) f.status();
        t.join();
    });
    f.shutdown();
}
