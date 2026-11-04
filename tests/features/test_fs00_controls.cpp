// FS-00 section 3 tests: ControlRegistry, built-in Core actions, /api/controls and
// OSC /shunt/action/<id>. Original Shunt code.
#include "TestFramework.h"
#include "shunt/app/Api.h"
#include "shunt/out/Osc.h"
#include <arpa/inet.h>
#include <chrono>
#include <cmath>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace shunt::app;

namespace {
HttpRequest req(const std::string& method, const std::string& path, const std::string& body = "") {
    HttpRequest r;
    r.method = method;
    r.path = path;
    r.body = body;
    return r;
}
template <class F> bool waitFor(F cond, int ms = 2000) {
    for (int i = 0; i < ms / 5; ++i) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}
int freeUdpPort() {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
    socklen_t l = sizeof a;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &l);
    ::close(fd);
    return ntohs(a.sin_port);
}
void sendUdp(int port, const std::vector<uint8_t>& data) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(uint16_t(port));
    ::sendto(fd, data.data(), data.size(), 0, reinterpret_cast<sockaddr*>(&a), sizeof a);
    ::close(fd);
}
Settings offlineSettings() {
    Settings s;
    s.interfaceName = "fs00-no-such-interface";   // no stack, no sockets on 50000-50002
    return s;
}
}

TEST_CASE("FS00-T2 control registry: add, replace, invoke, unknown, remove, clamp") {
    ControlRegistry reg;
    double got = -1;
    int calls = 0;
    ControlAction a;
    a.id = "x.go"; a.label = "Go"; a.kind = ControlAction::Value; a.group = "X";
    a.fn = [&](double v) { got = v; ++calls; };
    reg.add(a);
    CHECK(reg.has("x.go"));
    CHECK(reg.invoke("x.go", 0.25));
    CHECK_NEAR(got, 0.25, 1e-12);
    CHECK(reg.invoke("x.go", 7));          // clamped to 0..1
    CHECK_NEAR(got, 1.0, 1e-12);
    CHECK(reg.invoke("x.go", -3));
    CHECK_NEAR(got, 0.0, 1e-12);
    CHECK(reg.invoke("x.go", std::nan("")));
    CHECK_NEAR(got, 0.0, 1e-12);
    CHECK(!reg.invoke("nope", 1));
    CHECK_EQ(calls, 4);
    // replace keeps one entry with the new function and label
    int replaced = 0;
    ControlAction b = a;
    b.label = "Go again"; b.kind = ControlAction::Trigger;
    b.fn = [&](double) { ++replaced; };
    reg.add(b);
    CHECK(reg.invoke("x.go", 1));
    CHECK_EQ(replaced, 1);
    CHECK_EQ(calls, 4);
    auto l = reg.list();
    REQUIRE(l.size() == 1);
    CHECK(l[0].label == "Go again");
    CHECK(std::string(controlKindName(l[0].kind)) == "trigger");
    // invalid entries are ignored; list is sorted by id
    ControlAction bad; bad.id = ""; bad.fn = [](double) {};
    reg.add(bad);
    ControlAction nofn; nofn.id = "no.fn";
    reg.add(nofn);
    ControlAction c; c.id = "a.first"; c.fn = [](double) {};
    reg.add(c);
    l = reg.list();
    REQUIRE(l.size() == 2);
    CHECK(l[0].id == "a.first");
    reg.remove("x.go");
    CHECK(!reg.invoke("x.go", 1));
    CHECK_EQ(int(reg.list().size()), 1);
    // refused inside a Core-locked callback (would self-deadlock)
    {
        CoreLockedScope locked;
        CHECK(coreLockHeldByThisThread());
        CHECK(!reg.invoke("a.first", 1));
    }
    CHECK(!coreLockHeldByThisThread());
}

TEST_CASE("FS00-T3 built-in controls over /api/controls") {
    const std::string dir = "/tmp/shunt_fs00_ctl_" + std::to_string(::getpid());
    Core core(offlineSettings(), dir + "/config.json", dir);
    HttpResponse r = handleRequest(core, req("GET", "/api/controls"));
    CHECK_EQ(r.status, 200);
    Json list;
    REQUIRE(Json::parse(r.body, list));
    REQUIRE(list.isArray());
    const char* ids[] = {"baroffset.cycle", "baroffset.set.0", "baroffset.set.1", "baroffset.set.2", "baroffset.set.3", "beatonly.toggle",
                         "beatonly.on", "beatonly.off", "latency.up", "latency.down", "latency.set", "setstart.mark"};
    for (const char* id : ids) {
        bool found = false;
        for (auto& o : list.items()) {
            if (o.get("id").asString() != id) continue;
            found = true;
            CHECK(o.get("label").type() == Json::String);
            CHECK(o.get("group").type() == Json::String);
            const std::string k = o.get("kind").asString("");
            CHECK(k == "trigger" || k == "toggle" || k == "value");
        }
        if (!found) INFO("missing control %s\n", id);
        CHECK(found);
    }
    // latency.set: 0..1 maps to -20..+50 ms with 0.1 ms resolution
    handleRequest(core, req("POST", "/api/controls/latency.set", "{\"value\":0.5}"));
    CHECK_NEAR(core.settings().latencyMs, 15.0, 1e-9);
    handleRequest(core, req("POST", "/api/controls/latency.set", "{\"value\":7}"));   // clamped to 1
    CHECK_NEAR(core.settings().latencyMs, 50.0, 1e-9);
    handleRequest(core, req("POST", "/api/controls/latency.set", "{\"value\":0.30001}"));
    CHECK_NEAR(core.settings().latencyMs, 1.0, 1e-9);
    r = handleRequest(core, req("POST", "/api/controls/baroffset.set.2", "{\"value\":1}"));
    CHECK_EQ(r.status, 200);
    CHECK_EQ(core.settings().barOffset, 2);
    handleRequest(core, req("POST", "/api/controls/baroffset.cycle"));
    CHECK_EQ(core.settings().barOffset, 3);
    handleRequest(core, req("POST", "/api/controls/beatonly.toggle", "{}"));
    CHECK(core.settings().beatOnly);
    handleRequest(core, req("POST", "/api/controls/beatonly.off", "{}"));
    CHECK(!core.settings().beatOnly);
    const double lat = core.settings().latencyMs;
    handleRequest(core, req("POST", "/api/controls/latency.up", "{}"));
    CHECK_NEAR(core.settings().latencyMs, lat + 0.5, 1e-9);
    handleRequest(core, req("POST", "/api/controls/latency.down", "{}"));
    handleRequest(core, req("POST", "/api/controls/latency.down", "{}"));
    CHECK_NEAR(core.settings().latencyMs, lat - 0.5, 1e-9);
    CHECK_EQ(handleRequest(core, req("POST", "/api/controls/setstart.mark", "{}")).status, 200);
    Json st = core.status();
    CHECK(st.get("setStartMs").asNumber() > 0);
    CHECK_EQ(handleRequest(core, req("POST", "/api/controls/no.such", "{}")).status, 404);
    CHECK_EQ(handleRequest(core, req("POST", "/api/controls/beatonly.on", "{\"value\":\"x\"}")).status, 400);
    CHECK_EQ(handleRequest(core, req("POST", "/api/controls/beatonly.on", "{bad")).status, 400);
}

TEST_CASE("FS00-T3b OSC /shunt/action/<id> invokes controls; old addresses still work") {
    const std::string dir = "/tmp/shunt_fs00_osc_" + std::to_string(::getpid());
    Settings s = offlineSettings();
    s.oscInEnabled = true;
    s.oscInPort = freeUdpPort();
    Core core(s, dir + "/config.json", dir);
    core.start();
    using shunt::out::OscArg;
    sendUdp(s.oscInPort, shunt::out::oscMessage("/shunt/action/baroffset.set.3", {OscArg::Float(1.0f)}));
    CHECK(waitFor([&] { return core.config().get("barOffset").asInt() == 3; }));
    sendUdp(s.oscInPort, shunt::out::oscMessage("/shunt/action/beatonly.on", {}));
    CHECK(waitFor([&] { return core.config().get("beatOnly").asBool(); }));
    sendUdp(s.oscInPort, shunt::out::oscMessage("/shunt/beatonly", {OscArg::Int(0)}));
    CHECK(waitFor([&] { return !core.config().get("beatOnly").asBool(); }));
    sendUdp(s.oscInPort, shunt::out::oscMessage("/shunt/baroffset", {OscArg::Int(1)}));
    CHECK(waitFor([&] { return core.config().get("barOffset").asInt() == 1; }));
    sendUdp(s.oscInPort, shunt::out::oscMessage("/shunt/action/does.not.exist", {OscArg::Int(1)}));
    sendUdp(s.oscInPort, shunt::out::oscMessage("/shunt/action/baroffset.set.0", {OscArg::Int(1)}));
    CHECK(waitFor([&] { return core.config().get("barOffset").asInt() == 0; }));
    core.stop();
}
