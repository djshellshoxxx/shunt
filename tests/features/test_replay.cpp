// FS-05 Replay a capture tests (R-T1 .. R-T10, docs/specs/FS-05-replay-a-capture.md): pure
// transport with synthetic time against an unstarted NetworkStack + ClockEngine, the feature
// through Core and its HTTP routes, isolation and outputs gating (BUILD-PLAN R10), name and
// pcapng validation with truncation and mutation fuzz (FS-TEST section 4), real captures from
// tests/captures (skipped when absent; tools/fetch_captures.sh), a 3-thread hammer for TSan, and
// the HttpServer per-route body limit (R12). Original Shunt code.
#include "FixtureBuilder.h"
#include "TestFramework.h"
#include "../../app/src/features/Replay.h"
#include "shunt/app/Api.h"
#include "shunt/app/Http.h"
#include "shunt/clock/ClockEngine.h"
#include "shunt/net/NetworkStack.h"
#include "shunt/net/Packets.h"
#include "shunt/net/Socket.h"
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <netinet/in.h>
#include <random>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace shunt::app;
namespace net = shunt::net;
namespace clk = shunt::clock;
namespace rp = shunt::app::replay;

namespace {

constexpr int64_t kSec = 1'000'000'000LL;
constexpr int64_t kStepNs = 5'000'000;

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
void spit(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary);
    out << data;
}
template <class F> bool waitFor(F cond, int ms = 4000) {
    for (int i = 0; i < ms / 5; ++i) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

// 20 s, deck 1 master at 128 BPM, deck 2 playing at 124 BPM, master handoff to deck 2 at 10 s.
fixture::CaptureParams handoff20() {
    fixture::CaptureParams p;
    p.durationS = 20;
    fixture::DeckSpec d1; d1.number = 1; d1.bpm = 128; d1.master = true;
    fixture::DeckSpec d2; d2.number = 2; d2.bpm = 124; d2.master = false;
    p.decks = {d1, d2};
    p.changes.push_back({10.0, 1, [](fixture::DeckSpec& d) { d.master = false; }});
    p.changes.push_back({10.0, 2, [](fixture::DeckSpec& d) { d.master = true; }});
    return p;
}

std::shared_ptr<rp::Capture> captureOf(const fixture::CaptureParams& p) {
    auto c = std::make_shared<rp::Capture>();
    std::string e;
    if (!rp::buildCapture(fixture::buildCapture(p), *c, e)) return nullptr;
    return c;
}

// What Core does with a fed datagram, minus Core: unstarted stack (parsers only) and an engine.
struct EngineSink : rp::ReplaySink {
    std::unique_ptr<net::ISocketFactory> factory = net::makePosixSocketFactory();
    std::unique_ptr<net::NetworkStack> stack;
    clk::ClockEngine engine;
    int resets = 0, clears = 0, toTwo = 0;
    size_t feeds = 0;
    int64_t lastRecv = 0, lastWall = 0;
    bool monotonic = true;
    uint64_t parseErrors = 0;
    EngineSink() { make(); }
    void make() {
        net::StackConfig c;
        c.mode = net::Mode::Follow;
        c.filterSubnet = false;
        if (stack) parseErrors += stack->counters().parseErrors;
        stack = std::make_unique<net::NetworkStack>(*factory, c);
        engine = clk::ClockEngine();
    }
    void feed(uint16_t port, const uint8_t* data, size_t len, int64_t recvNs, uint32_t srcIp, int64_t wallMs) override {
        if (recvNs < lastRecv) monotonic = false;
        lastRecv = recvNs;
        lastWall = wallMs;
        ++feeds;
        stack->feed(port, data, len, recvNs, srcIp);
        stack->runTimers(recvNs);
        engine.tick(recvNs);
        net::Event ev;
        while (stack->pop(ev)) {
            engine.onEvent(ev);
            if (ev.type == net::EventType::MasterChanged && ev.masterChanged.to == 2) ++toTwo;
        }
    }
    void reset(bool clear) override { ++resets; clears += clear; make(); lastRecv = 0; }
    uint64_t totalParseErrors() const { return parseErrors + stack->counters().parseErrors; }
};

// Steps the player in 5 ms synthetic increments; cb(now) after every step, stop when it returns true.
template <class F> int64_t run(rp::ReplayPlayer& p, EngineSink& s, int64_t& now, int64_t forNs, F cb) {
    const int64_t end = now + forNs;
    while (now < end) {
        now += kStepNs;
        p.step(now, s);
        if (cb(now)) break;
    }
    return now;
}
int64_t run(rp::ReplayPlayer& p, EngineSink& s, int64_t& now, int64_t forNs) {
    return run(p, s, now, forNs, [](int64_t) { return false; });
}

// ---- Core rig -------------------------------------------------------------------------------
Settings offline() {
    Settings s;
    s.interfaceName = "fs05-no-such-interface";   // no live stack, no sockets on 50000-50002
    return s;
}

struct Rig {
    tf::TempDir dir;
    std::string caps;
    std::unique_ptr<Core> core;
    explicit Rig(bool enabled = true, Settings s = offline()) {
        caps = dir.file("captures");
        ::mkdir(caps.c_str(), 0755);
        Json f = Json::object();
        f["enabled"] = enabled;
        f["capturesDir"] = caps;
        s.raw["features"]["replay"] = f;
        core = std::make_unique<Core>(s, dir.file("config.json"), dir.path());
        core->start();
    }
    ~Rig() { core->stop(); }
    HttpResponse call(const std::string& method, const std::string& sub, const std::string& body = "") {
        HttpRequest r;
        r.method = method;
        const size_t q = sub.find('?');
        r.path = "/api/f/replay/" + sub.substr(0, q);
        if (q != std::string::npos) r.query = sub.substr(q + 1);
        r.body = body;
        return handleRequest(*core, r);
    }
    Json js(const HttpResponse& r) { Json j; Json::parse(r.body, j); return j; }
    Json st() { return core->status().get("features").get("replay"); }
    std::string put(const std::string& name, const fixture::CaptureParams& p) {
        const std::string path = caps + "/" + name;
        fixture::writeCapture(p, path);
        return path;
    }
};

std::string readAllFiles(const std::string& dir) {
    std::string all;
    if (DIR* d = ::opendir(dir.c_str())) {
        while (dirent* e = ::readdir(d)) {
            if (e->d_name[0] == '.') continue;
            all += slurp(dir + "/" + e->d_name);
        }
        ::closedir(d);
    }
    return all;
}

std::string capturesDir() {
    if (const char* p = std::getenv("SHUNT_CAPTURES_DIR")) if (*p) return p;
    std::string f = __FILE__;                         // .../tests/features/test_replay.cpp
    f = f.substr(0, f.find_last_of('/'));
    return f.substr(0, f.find_last_of('/')) + "/captures";
}

// Block boundaries of a valid pcapng buffer.
std::vector<size_t> boundaries(const std::string& b) {
    std::vector<size_t> out{0};
    size_t pos = 0;
    while (pos + 8 <= b.size()) {
        uint32_t len;
        std::memcpy(&len, b.data() + pos + 4, 4);
        pos += len;
        out.push_back(pos);
    }
    return out;
}

} // namespace

// ================================================================== pure transport

TEST_CASE("R-T1 handoff fixture at 8x: engine locks at 128, one handoff, then the new master's 124") {
    auto cap = captureOf(handoff20());
    REQUIRE(cap);
    CHECK_NEAR(double(cap->durationNs) / kSec, 20.0, 0.2);
    CHECK_EQ(cap->devices.size(), size_t(3));        // 2 CDJs + mixer from keep-alives
    rp::ReplayPlayer p;
    EngineSink s;
    int64_t now = 1000 * kSec;
    const int64_t t0 = now;
    p.load(cap, 8, false, now);
    bool lockedAt128 = false;
    run(p, s, now, 30 * kSec, [&](int64_t n) {
        const double pos = double(p.positionNs(n)) / kSec;
        const auto& tl = s.engine.timeline();
        if (pos > 6 && pos < 9.5 && s.engine.state() == clk::State::Locked && s.engine.master() == 1 && std::fabs(tl.bpm - 128) < 0.3)
            lockedAt128 = true;
        return p.ended();
    });
    CHECK(lockedAt128);
    CHECK(p.ended());
    CHECK_NEAR(double(now - t0) / kSec, 20.0 / 8, 0.05);   // 8x: 20 s of capture in 2.5 s
    CHECK_EQ(s.toTwo, 1);
    CHECK_EQ(int(s.engine.master()), 2);
    CHECK_NEAR(s.engine.timeline().bpm, 124.0, 0.3);
    CHECK(s.monotonic);
    CHECK_EQ(s.feeds, cap->dgrams.size());
    CHECK_EQ(s.clears, 1);                                   // start is a reset point
    CHECK_EQ(s.totalParseErrors(), uint64_t(0));
    CHECK_EQ(s.lastWall, (cap->firstTsNs + cap->durationNs) / 1'000'000);   // wall = capture clock
}

TEST_CASE("R-T2 pause freezes, resume continues, seek to 12 s gives the new master within 2 beats") {
    auto cap = captureOf(handoff20());
    REQUIRE(cap);
    rp::ReplayPlayer p;
    EngineSink s;
    int64_t now = 50 * kSec;
    p.load(cap, 1, false, now);
    run(p, s, now, 5 * kSec);
    CHECK_NEAR(double(p.positionNs(now)) / kSec, 5.0, 0.01);
    p.pause(now);
    const size_t fed = s.feeds;
    const int64_t recv = s.lastRecv;
    run(p, s, now, 2 * kSec);
    CHECK_NEAR(double(p.positionNs(now)) / kSec, 5.0, 0.01);
    CHECK_EQ(s.feeds, fed);
    CHECK(p.paused());
    p.resume(now);
    run(p, s, now, 1 * kSec);
    CHECK_NEAR(double(p.positionNs(now)) / kSec, 6.0, 0.01);
    CHECK(s.lastRecv - recv < int64_t(1.1 * kSec));          // the engine saw no 2 s gap: no time jump
    CHECK(s.engine.state() == clk::State::Locked);

    p.seek(12 * kSec, now);
    CHECK(p.resetPending());
    const int resets = s.resets;
    int64_t firstMasterNs = -1;
    const int64_t seekAt = now;
    run(p, s, now, 2 * kSec, [&](int64_t n) {
        if (firstMasterNs < 0 && s.engine.master() == 2) firstMasterNs = n;
        return false;
    });
    CHECK_EQ(s.resets, resets + 1);
    REQUIRE(firstMasterNs >= 0);
    CHECK(firstMasterNs - seekAt <= 2 * 468'750'000LL);      // within 2 beats at 128 BPM
    CHECK(s.engine.state() == clk::State::Locked);
    CHECK_NEAR(s.engine.timeline().bpm, 124.0, 0.3);
    CHECK_NEAR(double(p.positionNs(now)) / kSec, 14.0, 0.01);
    CHECK(s.monotonic);

    p.seek(-5 * kSec, now);                                  // clamped
    CHECK_EQ(p.positionNs(now), int64_t(0));
    p.seek(99 * kSec, now);
    CHECK_EQ(p.positionNs(now), cap->durationNs);
    p.setSpeed(3, now);                                      // snaps to an allowed speed
    CHECK_EQ(p.speed(), 4.0);
    CHECK_EQ(rp::snapSpeed(100), 8.0);
    CHECK_EQ(rp::snapSpeed(0.01), 0.25);
    CHECK_EQ(rp::snapSpeed(-1), 0.25);
    CHECK_EQ(rp::snapSpeed(0.7), 0.5);
    CHECK_EQ(rp::snapSpeed(1.5), 2.0);
    CHECK_EQ(rp::snapSpeed(std::nan("")), 1.0);
}

TEST_CASE("R-T3 loop wrap resets and keeps running; end without loop holds") {
    fixture::CaptureParams fp;
    fp.durationS = 8;
    auto cap = captureOf(fp);
    REQUIRE(cap);
    {
        rp::ReplayPlayer p;
        EngineSink s;
        int64_t now = 10 * kSec;
        p.load(cap, 8, true, now);
        run(p, s, now, int64_t(1.5 * kSec));                  // 12 s of an 8 s file
        CHECK_EQ(p.loops(), uint64_t(1));
        CHECK_EQ(s.clears, 2);                                // start + wrap
        CHECK(!p.ended());
        CHECK_NEAR(double(p.positionNs(now)) / kSec, 4.0, 0.1);
        const size_t fed = s.feeds;
        run(p, s, now, kSec / 4);
        CHECK(s.feeds > fed);                                 // keeps running after the wrap
        CHECK(s.monotonic);                                   // monotonic within each engine epoch
    }
    {
        rp::ReplayPlayer p;
        EngineSink s;
        int64_t now = 10 * kSec;
        p.load(cap, 8, false, now);
        run(p, s, now, int64_t(1.5 * kSec));
        CHECK(p.ended());
        CHECK_EQ(p.positionNs(now), cap->durationNs);
        CHECK_EQ(s.clears, 1);
        const size_t fed = s.feeds;
        const double bpm = s.engine.timeline().bpm;
        run(p, s, now, kSec);
        CHECK_EQ(s.feeds, fed);                               // holds
        CHECK_NEAR(s.engine.timeline().bpm, bpm, 1e-9);
        CHECK(bpm > 127 && bpm < 129);
        p.seek(0, now);                                       // Restart
        CHECK(!p.ended());
        run(p, s, now, kSec / 2);
        CHECK(s.feeds > fed);
        CHECK_EQ(s.clears, 2);
    }
}

TEST_CASE("R-T3b step budget and cost: bounded feeds per step, idle step is cheap") {
    fixture::CaptureParams fp;
    fp.durationS = 60;
    fixture::DeckSpec a; a.number = 1; a.master = true;
    fixture::DeckSpec b; b.number = 2;
    fixture::DeckSpec c; c.number = 3;
    fp.decks = {a, b, c};
    auto cap = captureOf(fp);
    REQUIRE(cap);
    rp::ReplayPlayer p;
    EngineSink s;
    int64_t now = 10 * kSec;
    p.load(cap, 1, false, now);
    p.step(now, s);
    p.seek(59 * kSec, now);                                  // 30 s warm-up window, budgeted
    CHECK(p.step(now, s, 100) == 100);
    size_t total = 100;
    for (int i = 0; i < 1000 && p.step(now, s, 100) == 100; ++i) total += 100;
    CHECK(total < cap->dgrams.size());                        // only the warm-up window, not the whole file
    CHECK(total > 500);
    p.pause(now);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) p.step(now + i, s);
    const double usPerStep = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 1000;
    INFO("idle step %.3f us\n", usPerStep);
    CHECK(usPerStep < 200);                                   // FS-TEST 5.7: 20 us budget with 10x CI slack
}

// ================================================================== validation

TEST_CASE("R-T6 names: bare .pcapng names only; upload names sanitised") {
    CHECK(rp::validName("set-2026_01.pcapng"));
    CHECK(rp::validName("a.pcapng"));
    CHECK(!rp::validName(".pcapng"));
    CHECK(!rp::validName("../x.pcapng"));
    CHECK(!rp::validName("/etc/x.pcapng"));
    CHECK(!rp::validName("a/b.pcapng"));
    CHECK(!rp::validName("a\\b.pcapng"));
    CHECK(!rp::validName(std::string("a\0b.pcapng", 10)));
    CHECK(!rp::validName("x.pcap"));
    CHECK(!rp::validName("x.txt"));
    CHECK(!rp::validName(".hidden.pcapng"));
    CHECK(!rp::validName("caf\xc3\xa9.pcapng"));
    CHECK(!rp::validName(std::string(130, 'a') + ".pcapng"));
    CHECK(!rp::validName(""));
    CHECK_STR(rp::sanitizeName("my set (live).pcapng"), "my_set__live_.pcapng");
    CHECK_STR(rp::sanitizeName("../../etc/passwd.pcapng"), "_.._etc_passwd.pcapng");
    CHECK_STR(rp::sanitizeName("..x.pcapng"), "x.pcapng");
    CHECK_STR(rp::sanitizeName("x.txt"), "");
    CHECK_STR(rp::sanitizeName("...."), "");
    CHECK_STR(rp::sanitizeName(""), "");
    CHECK_STR(rp::sanitizeName(std::string("a\0b.pcapng", 10)), "a_b.pcapng");
}

TEST_CASE("R-T6b pcapng check: clean errors for non-pcapng, pcap, big-endian, truncated, bad lengths") {
    auto dir = TEMP_DIR();
    const std::string path = dir.file("ok.pcapng");
    fixture::CaptureParams fp;
    fp.durationS = 1;
    REQUIRE(fixture::writeCapture(fp, path));
    const std::string good = slurp(path);
    auto chk = [](const std::string& b, uint64_t max = rp::kMaxUploadBytes) {
        return rp::checkPcapng(reinterpret_cast<const uint8_t*>(b.data()), b.size(), max);
    };
    auto r = chk(good);
    CHECK(r.ok);
    CHECK(r.packets > 5);
    CHECK(rp::checkPcapngFile(path, rp::kMaxFileBytes).ok);
    CHECK(chk("hello world, this is not a capture file at all").error.find("not a pcapng") != std::string::npos);
    std::string pcap = good; pcap[0] = char(0xd4); pcap[1] = char(0xc3); pcap[2] = char(0xb2); pcap[3] = char(0xa1);
    CHECK(chk(pcap).error.find("classic pcap") != std::string::npos);
    std::string be = good; be[8] = 0x1a; be[9] = 0x2b; be[10] = 0x3c; be[11] = 0x4d;
    CHECK(chk(be).error.find("big-endian") != std::string::npos);
    CHECK(chk(good.substr(0, good.size() - 3)).error.find("truncated") != std::string::npos);
    CHECK(chk(good.substr(0, 20)).error.find("too small") != std::string::npos);
    CHECK(chk(good, good.size() - 1).error.find("larger than") != std::string::npos);
    std::string odd = good; odd[4] = char(odd[4] + 1);
    CHECK(chk(odd).error.find("bad block length") != std::string::npos);
    std::string mism = good; mism[good.size() - 4] = char(mism[good.size() - 4] ^ 4);
    CHECK(chk(mism).error.find("mismatch") != std::string::npos);
    // EPB whose captured length points past the block (would make a naive reader read out of bounds)
    const auto bounds = boundaries(good);
    std::string over = good;
    for (size_t i = 0; i + 1 < bounds.size(); ++i) {
        uint32_t type;
        std::memcpy(&type, over.data() + bounds[i], 4);
        if (type != 6) continue;
        const uint32_t big = 0xfffffff0u;
        std::memcpy(&over[bounds[i] + 8 + 12], &big, 4);
        break;
    }
    CHECK(chk(over).error.find("longer than its block") != std::string::npos);
    rp::Capture c;
    std::string e;
    spit(dir.file("over.pcapng"), over);
    CHECK(!rp::loadCapture(dir.file("over.pcapng"), c, e));
    CHECK(!rp::loadCapture(dir.file("missing.pcapng"), c, e));
    CHECK_STR(e, "file not found");
    // valid pcapng with no player-network traffic
    std::vector<net::CapturedDatagram> other(1);
    other[0].timestampNs = 1; other[0].dstPort = 5353; other[0].payload = {1, 2, 3};
    REQUIRE(fixture::writeCapture(other, dir.file("mdns.pcapng")));
    CHECK(!rp::loadCapture(dir.file("mdns.pcapng"), c, e));
    CHECK(e.find("no player-network") != std::string::npos);
}

TEST_CASE("R-T6c fuzz: truncation at every byte and 2000 random mutations (no crash, no hang)") {
    WITH_TIMEOUT(120000, {
    auto dir = TEMP_DIR();
    fixture::CaptureParams fp;
    fp.durationS = 0.6;
    fp.mixer = false;
    const std::string path = dir.file("small.pcapng");
    REQUIRE(fixture::writeCapture(fp, path));
    const std::string good = slurp(path);
    INFO("fuzz corpus %zu bytes\n", good.size());
    REQUIRE(good.size() < 20000);
    const auto bounds = boundaries(good);
    auto chk = [](const std::string& b) { return rp::checkPcapng(reinterpret_cast<const uint8_t*>(b.data()), b.size(), rp::kMaxUploadBytes); };
    const std::string tmp = dir.file("t.pcapng");
    int okPrefixes = 0, loaded = 0;
    for (size_t n = 0; n < good.size(); ++n) {
        const std::string pre = good.substr(0, n);
        const auto r = chk(pre);
        const bool boundary = std::find(bounds.begin(), bounds.end(), n) != bounds.end() && n >= 28;
        if (r.ok != boundary) { CHECK(r.ok == boundary); INFO("prefix %zu: %s\n", n, r.error.c_str()); break; }
        if (r.ok) {
            ++okPrefixes;
            spit(tmp, pre);
            rp::Capture c;
            std::string e;
            loaded += rp::loadCapture(tmp, c, e);
        } else {
            CHECK(!r.error.empty());
        }
    }
    CHECK(okPrefixes >= 2);
    std::mt19937 rng(20260501);
    int accepted = 0;
    for (int it = 0; it < 2000; ++it) {
        std::string m = good;
        const int edits = 1 + int(rng() % 8);
        for (int k = 0; k < edits; ++k) {
            const size_t at = rng() % m.size();
            switch (rng() % 4) {
            case 0: m[at] = char(rng()); break;
            case 1: m[at] = char(m[at] ^ (1 << (rng() % 8))); break;
            case 2: { const uint32_t v = uint32_t(rng()); if (at + 4 <= m.size()) std::memcpy(&m[at], &v, 4); break; }
            default: m.resize(at + 1); break;
            }
        }
        const auto r = chk(m);
        if (!r.ok) { CHECK(!r.error.empty()); continue; }
        ++accepted;
        spit(tmp, m);
        rp::Capture c;
        std::string e;
        if (rp::loadCapture(tmp, c, e)) {
            CHECK(!c.dgrams.empty());
            for (auto& g : c.dgrams) CHECK(g.off + g.len <= c.arena.size());
            rp::ReplayPlayer p;                               // and it replays without trouble
            EngineSink s;
            int64_t now = 5 * kSec;
            p.load(std::make_shared<rp::Capture>(std::move(c)), 8, false, now);
            run(p, s, now, kSec / 4);
        }
    }
    INFO("prefixes ok %d (loaded %d), mutations accepted %d of 2000\n", okPrefixes, loaded, accepted);
    });
}

TEST_CASE("R-T6d real captures validate, load and replay (skipped when tests/captures is empty)") {
    struct Want { const char* file; size_t devices; };
    const Want wants[] = {{"powerup.pcapng", 3}, {"to-virtual.pcapng", 4}, {"S06-load-and-play.pcapng", 2}, {"Sync-Master.pcapng", 3}};
    int found = 0;
    for (const auto& w : wants) {
        const std::string path = capturesDir() + "/" + w.file;
        if (::access(path.c_str(), R_OK) != 0) { std::printf("  SKIP: %s not present (tools/fetch_captures.sh)\n", path.c_str()); continue; }
        ++found;
        const auto chk = rp::checkPcapngFile(path, rp::kMaxFileBytes);
        CHECK(chk.ok);
        if (!chk.ok) INFO("%s: %s\n", w.file, chk.error.c_str());
        auto cap = std::make_shared<rp::Capture>();
        std::string e;
        REQUIRE(rp::loadCapture(path, *cap, e));
        CHECK_EQ(cap->devices.size(), w.devices);
        rp::ReplayPlayer p;
        EngineSink s;
        int64_t now = 100 * kSec;
        p.load(cap, 8, false, now);
        int64_t guard = 0;
        while (!p.ended() && ++guard < 10'000'000) { now += 20'000'000; p.step(now, s, 100000); }
        CHECK(p.ended());
        CHECK_EQ(s.feeds, cap->dgrams.size());
        CHECK_EQ(s.totalParseErrors(), uint64_t(0));
        CHECK(s.monotonic);
        INFO("%s: %zu datagrams (%llu other), %.1f s, master %d, bpm %.2f\n", w.file, cap->dgrams.size(),
             (unsigned long long)cap->ignored, double(cap->durationNs) / kSec, int(s.engine.master()), s.engine.timeline().bpm);
        if (std::string(w.file) == "S06-load-and-play.pcapng") {   // same golden as replay_s06_load_and_play
            CHECK_EQ(int(s.engine.master()), 1);
            CHECK(s.engine.timeline().bpm > 127 && s.engine.timeline().bpm < 129);
        }
    }
    if (!found) std::printf("  SKIP: no real captures in %s\n", capturesDir().c_str());
}

namespace {
std::string simPath() {
    if (const char* p = std::getenv("SHUNTSIM_PATH")) if (*p) return p;
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return "";
    std::string exe(buf, size_t(n));
    return exe.substr(0, exe.find_last_of('/')) + "/shuntsim";
}
}

TEST_CASE("R-T11 shuntsim --record longset (20 min) replays at 8x: both handoffs, synthetic 150 s") {
    WITH_TIMEOUT(120000, {
    const std::string sim = simPath();
    std::string launcher;
    if (const char* l = std::getenv("SHUNT_SIM_LAUNCHER")) launcher = std::string(l) + " ";
    if (sim.empty() || ::access(sim.c_str(), X_OK) != 0 || std::system((launcher + sim + " --help >/dev/null 2>&1").c_str()) != 0) {
        std::printf("  SKIP: shuntsim not runnable here\n");
        return;
    }
    auto dir = TEMP_DIR();
    const std::string pcap = dir.file("longset.pcapng");
    REQUIRE(std::system((launcher + sim + " --scenario longset --time-scale 0 --quiet --record " + pcap + " >/dev/null 2>&1").c_str()) == 0);
    auto cap = std::make_shared<rp::Capture>();
    std::string e;
    REQUIRE(rp::loadCapture(pcap, *cap, e));
    CHECK_NEAR(double(cap->durationNs) / kSec, 1200.0, 2.0);
    rp::ReplayPlayer p;
    EngineSink s;
    std::vector<int> masters;
    int64_t now = 10 * kSec;
    const int64_t t0 = now;
    p.load(cap, 8, false, now);
    while (!p.ended() && now - t0 < 400 * kSec) {
        now += 20'000'000;
        p.step(now, s, 100000);
        const int m = s.engine.master();
        if (m && (masters.empty() || masters.back() != m)) masters.push_back(m);
    }
    CHECK(p.ended());
    CHECK_NEAR(double(now - t0) / kSec, 150.0, 0.5);         // 20 min at 8x
    CHECK(masters == std::vector<int>({1, 2, 1}));
    CHECK_EQ(s.feeds, cap->dgrams.size());
    CHECK_EQ(s.totalParseErrors(), uint64_t(0));
    CHECK(s.monotonic);
    });
}

// ================================================================== feature through Core

TEST_CASE("R-T1b feature at 8x through Core: locks at 128, hands off to 124, ends") {
    WITH_TIMEOUT(20000, {
    Rig rig;
    rig.put("handoff.pcapng", handoff20());
    auto r = rig.call("POST", "start", "{\"file\":\"handoff.pcapng\",\"speed\":8}");
    REQUIRE(r.status == 200);
    bool saw128 = false, saw124 = false, sawLocked = false;
    int rowsDuring = 0;
    CHECK(waitFor([&] {
        Json s = rig.core->status();
        const double bpm = s.get("bpm").asNumber();
        if (s.get("state").asString() == "locked") sawLocked = true;
        if (s.get("master").asInt() == 1 && std::fabs(bpm - 128) < 0.3) saw128 = true;
        if (saw128 && s.get("master").asInt() == 2 && std::fabs(bpm - 124) < 0.3) saw124 = true;
        return s.get("features").get("replay").get("ended").asBool();
    }, 6000));
    CHECK(sawLocked);
    CHECK(saw128);
    CHECK(saw124);
    Json st = rig.st();
    CHECK_NEAR(st.get("positionS").asNumber(), st.get("durationS").asNumber(), 1e-6);
    CHECK(rig.core->status().get("claim").asString() == "replay");
    rowsDuring = int(rig.core->tracklist().get("rows").size());
    CHECK(rowsDuring >= 1);
    CHECK(rig.call("POST", "stop").status == 200);
    CHECK(!rig.st().get("active").asBool());
    CHECK(rig.core->status().get("claim").asString() != "replay");
    });
}

TEST_CASE("R-T4 isolation: no sockets and nothing sent during replay, live session restored, rows not persisted") {
    WITH_TIMEOUT(20000, {
    Rig rig;
    rig.put("h.pcapng", handoff20());
    const Json before = rig.core->status();
    const int liveEvents = before.get("events").asInt();
    REQUIRE(rig.call("POST", "start", "{\"file\":\"h.pcapng\",\"speed\":8}").status == 200);
    CHECK(waitFor([&] { return rig.core->tracklist().get("rows").size() > 0; }));
    Json s = rig.core->status();
    CHECK_EQ(s.get("counters").get("sent").asNumber(), 0.0);   // the replay stack is never started
    CHECK(s.get("claim").asString() == "replay");
    CHECK(rig.call("POST", "stop").status == 200);
    s = rig.core->status();
    CHECK_EQ(s.get("events").asInt(), liveEvents);
    CHECK_EQ(rig.core->tracklist().get("rows").size(), size_t(0));
    rig.core->stop();
    const std::string files = readAllFiles(rig.dir.file("tracklists"));
    CHECK(files.find("1001") == std::string::npos);            // fixture track ids 1001 / 1002
    CHECK(files.find("1002") == std::string::npos);
    });
}

namespace {
// UDP bind probe on 0.0.0.0:<port>, no reuse flags: succeeds only when nobody holds the port.
bool portFree(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    const bool ok = ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
    ::close(fd);
    return ok;
}
}

TEST_CASE("R-T4b loopback live stack: replay releases 50000-50002, stop restores the live stack within 1 s") {
    WITH_TIMEOUT(20000, {
    if (!portFree(50000) || !portFree(50001) || !portFree(50002)) { std::printf("  SKIP: ports 50000-50002 busy\n"); return; }
    Settings s;
    s.interfaceName = "127.0.0.1";
    Rig rig(true, s);
    if (!waitFor([&] { Json st = rig.core->status(); return st.get("engineError").asString().empty() && !portFree(50001); }, 2000)) {
        std::printf("  SKIP: live stack did not come up on loopback\n");
        return;
    }
    rig.put("h.pcapng", handoff20());
    REQUIRE(rig.call("POST", "start", "{\"file\":\"h.pcapng\",\"speed\":1}").status == 200);
    CHECK(waitFor([&] { return portFree(50000) && portFree(50001) && portFree(50002); }, 1000));   // no sockets
    CHECK_EQ(rig.core->status().get("counters").get("sent").asNumber(), 0.0);
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(rig.call("POST", "stop").status == 200);
    CHECK(waitFor([&] {
        Json st = rig.core->status();
        return st.get("claim").asString() != "replay" && st.get("engineError").asString().empty() && st.get("interface").asString() != "";
    }, 1000));
    CHECK(waitFor([&] { return !portFree(50001); }, 1000));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    INFO("live stack back after %.0f ms\n", ms);
    CHECK(ms < 1000);
    });
}

TEST_CASE("R-T5 outputs off by default; on only when asked and only at speed 1") {
    WITH_TIMEOUT(30000, {
    Settings s = offline();
    s.outputs.osc.enabled = true;
    s.outputs.osc.host = "127.0.0.1";
    s.outputs.osc.port = 9;                 // discard port; only the sent counter matters
    Rig rig(true, s);
    rig.put("h.pcapng", handoff20());
    auto oscSent = [&] { return rig.core->status().get("outputs").get("osc").get("sent").asNumber(); };
    REQUIRE(rig.call("POST", "start", "{\"file\":\"h.pcapng\",\"speed\":8}").status == 200);
    CHECK(!rig.st().get("outputs").asBool());
    CHECK(waitFor([&] { return rig.st().get("ended").asBool(); }, 6000));
    CHECK(rig.core->status().get("state").asString() == "locked");
    CHECK_EQ(oscSent(), 0.0);
    CHECK_EQ(rig.call("POST", "start", "{\"file\":\"h.pcapng\",\"speed\":2,\"outputs\":true}").status, 400);
    CHECK_EQ(rig.call("POST", "start", "{\"file\":\"h.pcapng\",\"outputs\":\"yes\"}").status, 400);
    REQUIRE(rig.call("POST", "start", "{\"file\":\"h.pcapng\",\"speed\":1,\"outputs\":true}").status == 200);
    CHECK(rig.st().get("outputs").asBool());
    CHECK_EQ(rig.call("POST", "set", "{\"speed\":4}").status, 400);   // outputs pin speed 1
    CHECK(waitFor([&] { return oscSent() > 0; }, 10000));
    rig.call("POST", "stop");
    const double after = oscSent();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(waitFor([&] { return oscSent() == after; }, 500));       // no live stack: nothing more
    });
}

TEST_CASE("R-T6e routes reject traversal, bad names, non-pcapng and truncated files; uploads validated") {
    Rig rig;
    rig.put("ok.pcapng", handoff20());
    auto code = [&](const std::string& body) { return rig.call("POST", "start", body).status; };
    CHECK_EQ(code("{\"file\":\"../ok.pcapng\"}"), 400);
    CHECK_EQ(code("{\"file\":\"/etc/passwd\"}"), 400);
    CHECK_EQ(code("{\"file\":\"ok\\u0000.pcapng\"}"), 400);
    CHECK_EQ(code("{\"file\":\"captures/ok.pcapng\"}"), 400);
    CHECK_EQ(code("{\"file\":42}"), 400);
    CHECK_EQ(code("{}"), 400);
    CHECK_EQ(code("not json"), 400);
    CHECK_EQ(code("[1]"), 400);
    CHECK_EQ(code("{\"file\":\"missing.pcapng\"}"), 404);
    spit(rig.caps + "/text.pcapng", "this is not a capture, just text, long enough to pass the size check");
    auto r = rig.call("POST", "start", "{\"file\":\"text.pcapng\"}");
    CHECK_EQ(r.status, 422);
    CHECK(r.body.find("not a pcapng") != std::string::npos);
    const std::string good = slurp(rig.caps + "/ok.pcapng");
    spit(rig.caps + "/cut.pcapng", good.substr(0, good.size() - 100));
    r = rig.call("POST", "start", "{\"file\":\"cut.pcapng\"}");
    CHECK_EQ(r.status, 422);
    CHECK(r.body.find("truncated") != std::string::npos);
    CHECK(!rig.st().get("active").asBool());
    CHECK_EQ(rig.call("GET", "info?file=..%2Fx.pcapng").status, 400);
    CHECK_EQ(rig.call("GET", "info?file=cut.pcapng").status, 200);
    CHECK(rig.js(rig.call("GET", "info?file=cut.pcapng")).get("info").get("error").asString().find("truncated") != std::string::npos);

    // uploads
    r = rig.call("POST", "upload?name=my%20set.pcapng", good);
    CHECK_EQ(r.status, 201);
    CHECK_STR(rig.js(r).get("name").asString(), "my_set.pcapng");   // percent-decoded, then sanitised
    CHECK(slurp(rig.caps + "/my_set.pcapng") == good);
    CHECK_EQ(rig.call("POST", "upload?name=my%20set.pcapng", good).status, 409);              // no overwrite
    CHECK_EQ(rig.call("POST", "upload?name=my%20set.pcapng&replace=1", good).status, 201);
    CHECK_EQ(rig.call("POST", "upload?name=x.txt", good).status, 400);
    CHECK_EQ(rig.call("POST", "upload?name=..", good).status, 400);
    CHECK_EQ(rig.call("POST", "upload", good).status, 400);
    r = rig.call("POST", "upload?name=bad.pcapng", good.substr(0, good.size() - 7));
    CHECK_EQ(r.status, 422);
    CHECK(::access((rig.caps + "/bad.pcapng").c_str(), F_OK) != 0);
    CHECK_EQ(rig.call("POST", "upload?name=..%2F..%2Fescape.pcapng", good).status, 201);         // sanitised into the folder
    CHECK(::access(rig.dir.file("escape.pcapng").c_str(), F_OK) != 0);
    CHECK(::access((rig.caps + "/_.._escape.pcapng").c_str(), F_OK) == 0);
    bool tmpLeft = false;
    if (DIR* d = ::opendir(rig.caps.c_str())) { while (dirent* e = ::readdir(d)) if (std::strstr(e->d_name, ".tmp")) tmpLeft = true; ::closedir(d); }
    CHECK(!tmpLeft);
    // uploaded file plays
    CHECK_EQ(rig.call("POST", "start", "{\"file\":\"my_set.pcapng\",\"speed\":8}").status, 200);
    rig.call("POST", "stop");
}

TEST_CASE("R-T7 API shapes and errors") {
    Rig rig;
    rig.put("a.pcapng", handoff20());
    Json st = rig.st();
    for (const char* k : {"active", "paused", "loop", "ended", "outputs", "enabled"}) CHECK_JSON_TYPE(st, k, Json::Bool);
    for (const char* k : {"positionS", "durationS", "speed", "loops", "datagrams", "fed"}) CHECK_JSON_TYPE(st, k, Json::Number);
    for (const char* k : {"file", "lastError", "error"}) CHECK_JSON_TYPE(st, k, Json::String);
    CHECK_JSON(st, "active", false);
    CHECK_JSON(st, "enabled", true);

    auto files = rig.call("GET", "files");
    CHECK_EQ(files.status, 200);
    Json fj = rig.js(files);
    CHECK_JSON(fj, "dir", rig.caps);
    CHECK_JSON(fj, "maxUploadBytes", double(rp::kMaxUploadBytes));
    REQUIRE(fj.get("files").size() == 1);
    const Json& f0 = fj.get("files").items()[0];
    CHECK_JSON(f0, "name", "a.pcapng");
    CHECK_JSON_TYPE(f0, "size", Json::Number);
    CHECK_JSON_TYPE(f0, "mtimeMs", Json::Number);
    CHECK(f0.get("info").isNull());                          // lazy: not scanned yet
    Json info = rig.js(rig.call("GET", "info?file=a.pcapng"));
    CHECK_JSON(info, "name", "a.pcapng");
    CHECK_JSON_TYPE(info, "info.datagrams", Json::Number);
    CHECK_NEAR(info.get("info").get("durationS").asNumber(), 20.0, 0.2);
    CHECK_EQ(info.get("info").get("devices").size(), size_t(3));
    CHECK_JSON(info, "info.devices[0].number", 1);
    CHECK_JSON(info, "info.error", "");
    CHECK(!rig.js(rig.call("GET", "files")).get("files").items()[0].get("info").isNull());   // cached now

    CHECK_EQ(rig.call("POST", "pause").status, 409);           // nothing running
    CHECK_EQ(rig.call("POST", "seek", "{\"t\":1}").status, 409);
    CHECK_EQ(rig.call("POST", "stop").status, 200);            // idempotent
    CHECK_EQ(rig.call("GET", "start").status, 405);
    CHECK_EQ(rig.call("POST", "files").status, 405);
    CHECK_EQ(rig.call("GET", "nope").status, 404);

    auto r = rig.call("POST", "start", "{\"file\":\"a.pcapng\",\"speed\":0.5,\"loop\":true}");
    REQUIRE(r.status == 200);
    Json sj = rig.js(r);
    CHECK_JSON(sj, "active", true);
    CHECK_JSON(sj, "file", "a.pcapng");
    CHECK_JSON(sj, "speed", 0.5);
    CHECK_JSON(sj, "loop", true);
    CHECK_JSON(sj, "outputs", false);
    CHECK_NEAR(sj.get("durationS").asNumber(), 20.0, 0.2);
    CHECK(waitFor([&] { return rig.st().get("fed").asNumber() > 0; }));
    CHECK_JSON(rig.js(rig.call("POST", "pause")), "paused", true);
    const double p0 = rig.st().get("positionS").asNumber();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK_NEAR(rig.st().get("positionS").asNumber(), p0, 1e-9);
    CHECK_EQ(rig.call("POST", "seek", "{\"t\":\"x\"}").status, 400);
    CHECK_NEAR(rig.js(rig.call("POST", "seek", "{\"t\":1e9}")).get("positionS").asNumber(), sj.get("durationS").asNumber(), 1e-6);   // clamped
    CHECK_NEAR(rig.js(rig.call("POST", "seek", "{\"t\":-3}")).get("positionS").asNumber(), 0.0, 1e-9);
    CHECK_JSON(rig.js(rig.call("POST", "set", "{\"speed\":3,\"loop\":false}")), "speed", 4.0);
    CHECK_JSON(rig.st(), "loop", false);
    CHECK_EQ(rig.call("POST", "set", "{\"loop\":1}").status, 400);
    CHECK_JSON(rig.js(rig.call("POST", "resume")), "paused", false);
    CHECK_JSON(rig.js(rig.call("GET", "status")), "active", true);
    CHECK_EQ(rig.call("POST", "start", "{\"file\":\"a.pcapng\",\"speed\":8}").status, 200);    // restart while active
    CHECK_JSON(rig.st(), "speed", 8.0);
    CHECK_EQ(rig.call("POST", "seek", std::string(5000, ' ')).status, 413);
    CHECK_EQ(rig.call("POST", "stop").status, 200);
    CHECK_JSON(rig.st(), "active", false);
}

TEST_CASE("R-T10 disabled by default and inert") {
    Rig rig(false);
    rig.put("a.pcapng", handoff20());
    Json st = rig.st();
    CHECK_JSON(st, "enabled", false);
    CHECK_JSON(st, "active", false);
    CHECK_EQ(rig.call("POST", "start", "{\"file\":\"a.pcapng\"}").status, 409);
    CHECK_EQ(rig.call("POST", "upload?name=b.pcapng", slurp(rig.caps + "/a.pcapng")).status, 409);
    CHECK(::access((rig.caps + "/b.pcapng").c_str(), F_OK) != 0);
    CHECK(rig.core->status().get("claim").asString() != "replay");
    CHECK_EQ(rig.call("GET", "files").status, 200);
    bool found = false;
    const Json feats = rig.core->features();
    for (auto& f : feats.items())
        if (f.get("id").asString() == "replay") { found = true; CHECK(!f.get("enabled").asBool()); }
    CHECK(found);
    // also default-off with no settings at all
    tf::TempDir d2;
    Core c2(offline(), d2.file("config.json"), d2.path());
    c2.start();
    CHECK_JSON(c2.status().get("features").get("replay"), "enabled", false);
    c2.stop();
    // enabling then disabling through /api/config stops a running replay
    Rig on;
    on.put("a.pcapng", handoff20());
    REQUIRE(on.call("POST", "start", "{\"file\":\"a.pcapng\"}").status == 200);
    std::string e;
    Json patch;
    Json::parse("{\"features\":{\"replay\":{\"enabled\":false}}}", patch);
    CHECK(on.core->applyConfig(patch, e));
    CHECK(!on.st().get("active").asBool());
    CHECK(on.core->status().get("claim").asString() != "replay");
}

TEST_CASE("R-T8 start/stop/seek/pause/status hammered from 3 threads (TSan)") {
    WITH_TIMEOUT(30000, {
    Rig rig;
    fixture::CaptureParams fp;
    fp.durationS = 6;
    rig.put("s.pcapng", fp);
    std::atomic<bool> stop{false};
    std::atomic<int> ops{0};
    std::thread a([&] {
        while (!stop) {
            rig.call("POST", "start", "{\"file\":\"s.pcapng\",\"speed\":8,\"loop\":true}");
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            rig.call("POST", "stop");
            ++ops;
        }
    });
    std::thread b([&] {
        int i = 0;
        while (!stop) {
            switch (i++ % 5) {
            case 0: rig.call("POST", "seek", "{\"t\":" + std::to_string(i % 6) + "}"); break;
            case 1: rig.call("POST", "pause"); break;
            case 2: rig.call("POST", "resume"); break;
            case 3: rig.call("POST", "set", "{\"speed\":2}"); break;
            default: rig.call("GET", "files"); break;
            }
            ++ops;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    std::thread c([&] {
        while (!stop) {
            (void)rig.core->status();
            rig.call("GET", "status");
            (void)rig.core->tracklist();
            ++ops;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    stop = true;
    a.join(); b.join(); c.join();
    INFO("%d operations\n", ops.load());
    CHECK(ops.load() > 50);
    rig.call("POST", "stop");
    CHECK(!rig.st().get("active").asBool());
    CHECK(rig.core->status().get("claim").asString() != "replay");
    });
}

// ================================================================== HttpServer per-route body limit (R12)

namespace {
// Minimal HTTP/1.1 client: sends headers (+ optional body bytes) and returns the status code.
int rawPost(int port, const std::string& path, size_t contentLength, const std::string* body) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(uint16_t(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { ::close(fd); return -1; }
    const std::string head = "POST " + path + " HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(contentLength) + "\r\n\r\n";
    ::send(fd, head.data(), head.size(), MSG_NOSIGNAL);
    if (body) {
        size_t off = 0;
        while (off < body->size()) {
            const ssize_t w = ::send(fd, body->data() + off, std::min<size_t>(1 << 20, body->size() - off), MSG_NOSIGNAL);
            if (w <= 0) break;
            off += size_t(w);
        }
    }
    std::string resp;
    char buf[4096];
    for (;;) {
        const ssize_t r = ::recv(fd, buf, sizeof buf, 0);
        if (r <= 0) break;
        resp.append(buf, size_t(r));
    }
    ::close(fd);
    if (resp.compare(0, 9, "HTTP/1.1 ") != 0) return -1;
    return std::atoi(resp.c_str() + 9);
}
}

TEST_CASE("R-T9 HttpServer per-route body limit: default, prefix override, replay upload 256 MiB") {
    WITH_TIMEOUT(30000, {
    {
        HttpServer probe;
        CHECK_EQ(probe.maxBodyFor("/api/config"), HttpServer::kDefaultMaxBody);
        CHECK_EQ(probe.maxBodyFor("/api/rekordbox-xml"), size_t(64) << 20);
        CHECK_EQ(probe.maxBodyFor("/api/f/replay/upload"), size_t(256) << 20);
        CHECK_EQ(probe.maxBodyFor("/api/f/replay/start"), HttpServer::kDefaultMaxBody);
        probe.setMaxBody("/api/f/", 1000);
        CHECK_EQ(probe.maxBodyFor("/api/f/replay/upload"), size_t(256) << 20);   // longest prefix wins
        CHECK_EQ(probe.maxBodyFor("/api/f/other"), size_t(1000));
        probe.setMaxBody("/api/f/", 2000);                                       // same prefix replaces
        CHECK_EQ(probe.maxBodyFor("/api/f/other"), size_t(2000));
        probe.setMaxBody(10);
        CHECK_EQ(probe.maxBodyFor("/x"), size_t(10));
    }
    HttpServer server;
    server.setMaxBody(size_t(1) << 20);
    server.setMaxBody("/big", size_t(3) << 20);
    std::atomic<size_t> lastBody{0};
    REQUIRE(server.start("127.0.0.1", 0, [&](const HttpRequest& r) {
        lastBody = r.body.size();
        HttpResponse resp;
        resp.body = "{}";
        return resp;
    }));
    const std::string two(size_t(2) << 20, 'x');
    CHECK_EQ(rawPost(server.port(), "/small", two.size(), nullptr), 413);        // refused before the body
    CHECK_EQ(rawPost(server.port(), "/big", two.size(), &two), 200);
    CHECK_EQ(lastBody.load(), two.size());
    CHECK_EQ(rawPost(server.port(), "/big/sub?x=1", two.size(), &two), 200);
    CHECK_EQ(rawPost(server.port(), "/big", size_t(4) << 20, nullptr), 413);
    const std::string small(100, 's');
    CHECK_EQ(rawPost(server.port(), "/small", small.size(), &small), 200);       // under the default
    CHECK_EQ(lastBody.load(), small.size());
    server.stop();

    HttpServer def;                                                               // the defaults the app uses
    REQUIRE(def.start("127.0.0.1", 0, [&](const HttpRequest& r) { lastBody = r.body.size(); return HttpResponse{}; }));
    CHECK_EQ(rawPost(def.port(), "/api/rekordbox-xml", (size_t(64) << 20) + 1, nullptr), 413);
    CHECK_EQ(rawPost(def.port(), "/api/f/replay/upload", (size_t(256) << 20) + 1, nullptr), 413);
    const std::string big((size_t(64) << 20) + 4096, 'y');                       // over the default, under 256 MiB
    CHECK_EQ(rawPost(def.port(), "/api/f/replay/upload?name=a.pcapng", big.size(), &big), 200);
    CHECK_EQ(lastBody.load(), big.size());
    def.stop();
    });
}
