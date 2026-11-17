// RS-07 app tests A-T1 .. A-T3.
#include "TestFramework.h"
#include "shunt/app/Api.h"
#include <arpa/inet.h>
#include <chrono>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace shunt::app;

namespace {
std::string httpGet(int port, const std::string& req, int wsFrames = 0) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(uint16_t(port)); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { ::close(fd); return ""; }
    timeval tv{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::send(fd, req.data(), req.size(), 0);
    std::string out;
    char buf[4096];
    for (;;) {
        ssize_t r = ::recv(fd, buf, sizeof buf, 0);
        if (r <= 0) break;
        out.append(buf, size_t(r));
        if (wsFrames && out.find("\r\n\r\n") != std::string::npos && out.size() > out.find("\r\n\r\n") + 6) break;
    }
    ::close(fd);
    return out;
}
std::string body(const std::string& resp) { auto p = resp.find("\r\n\r\n"); return p == std::string::npos ? "" : resp.substr(p + 4); }
}

TEST_CASE("A-T1 JSON round trip, escapes and errors") {
    Json j;
    std::string err;
    REQUIRE(Json::parse("{\"a\":1,\"b\":[true,null,\"x\\n\\u00e9\"],\"c\":{\"d\":2.5}}", j, &err));
    CHECK_EQ(j.get("a").asInt(), 1);
    CHECK(j.get("b").items()[0].asBool());
    CHECK(j.get("b").items()[2].asString() == "x\n\xc3\xa9");
    CHECK_NEAR(j.get("c").get("d").asNumber(), 2.5, 1e-9);
    Json k;
    REQUIRE(Json::parse(j.dump(), k));
    CHECK(k.dump() == j.dump());
    CHECK(!Json::parse("{\"a\":", k, &err));
    CHECK(!Json::parse("[1,2] x", k, &err));
}

TEST_CASE("A-T1b settings: clamp, preserve unknown keys, save and load") {
    Json j;
    REQUIRE(Json::parse("{\"latencyMs\":999,\"barOffset\":6,\"deviceNumber\":9,\"future\":{\"x\":1},\"osc\":{\"port\":99999,\"profile\":\"qlab\"}}", j));
    Settings s = Settings::fromJson(j);
    CHECK_NEAR(s.latencyMs, 50.0, 1e-9);
    CHECK_EQ(s.barOffset, 2);
    CHECK_EQ(s.deviceNumber, 6);
    CHECK_EQ(int(s.outputs.osc.port), 65535);
    CHECK(s.outputs.osc.profile == shunt::out::OscProfile::QLab);
    const std::string path = "/tmp/shunt_test_cfg_" + std::to_string(::getpid()) + "/config.json";
    std::string err;
    REQUIRE(s.save(path, &err));
    Settings t = Settings::load(path);
    CHECK_EQ(t.barOffset, 2);
    CHECK(t.toJson().get("future").get("x").asInt() == 1);
    ::unlink(path.c_str());
}

TEST_CASE("A-T2 WebSocket helpers match RFC 6455") {
    CHECK(wsAcceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    CHECK(base64("Man") == "TWFu");
    CHECK(base64("Ma") == "TWE=");
    std::string f = wsEncodeText("hello");
    CHECK_EQ(int(uint8_t(f[0])), 0x81);
    CHECK_EQ(int(uint8_t(f[1])), 5);
    // masked client frame from RFC 6455 section 5.7: "Hello"
    const std::string masked("\x81\x85\x37\xfa\x21\x3d\x7f\x9f\x4d\x51\x58", 11);
    int op = 0; std::string pl;
    CHECK_EQ(wsDecode(masked, op, pl), 11);
    CHECK(pl == "Hello");
    CHECK_EQ(wsDecode(masked.substr(0, 5), op, pl), 0);
    std::string big(70000, 'x');
    std::string bf = wsEncodeText(big);
    CHECK_EQ(int(uint8_t(bf[1])), 127);
}

TEST_CASE("A-T3 HTTP/WebSocket server and Core API on loopback") {
    const std::string dir = "/tmp/shunt_test_core_" + std::to_string(::getpid());
    Settings s;
    s.interfaceName = "127.0.0.1";
    Core core(s, dir + "/config.json", dir);
    HttpServer server;
    REQUIRE(server.start("127.0.0.1", 0, [&](const HttpRequest& r) { return handleRequest(core, r); }));
    server.onWsOpen = [&] { return core.status().dump(); };
    std::string err;
    core.start(&err);
    const int port = server.port();

    std::string r = httpGet(port, "GET /api/status HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(r.find("200 OK") != std::string::npos);
    Json st;
    REQUIRE(Json::parse(body(r), st));
    CHECK(st.get("state").asString() == "idle");
    CHECK(st.get("outputs").get("osc").has("enabled"));

    r = httpGet(port, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(r.find("text/html") != std::string::npos);
    CHECK(body(r).find("<title>Shunt</title>") != std::string::npos);

    const std::string post = "{\"value\":2}";
    r = httpGet(port, "POST /api/baroffset HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(post.size()) + "\r\n\r\n" + post);
    CHECK(r.find("200 OK") != std::string::npos);
    CHECK_EQ(core.settings().barOffset, 2);

    r = httpGet(port, "POST /api/config HTTP/1.1\r\nHost: x\r\nContent-Length: 8\r\n\r\n{\"osc\":{");
    CHECK(r.find("400") != std::string::npos);

    r = httpGet(port, "GET /api/export?format=csv HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(r.find("attachment") != std::string::npos);
    r = httpGet(port, "GET /api/export?format=bogus HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(r.find("400") != std::string::npos);
    r = httpGet(port, "GET /nope HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(r.find("404") != std::string::npos);

    r = httpGet(port, "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n", 1);
    CHECK(r.find("101 Switching") != std::string::npos);
    CHECK(r.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
    CHECK(r.find("\"type\":\"status\"") != std::string::npos);

    server.stop();
    core.stop();
}
