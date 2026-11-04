// Tests for the T0 test infrastructure (FS-TEST sections 2 and 3): TestFramework helpers, golden
// compare, FixtureBuilder and the determinism/timeline of every shuntsim scenario (run as a
// subprocess with --record and --time-scale 0, so no sockets and no waiting).
// Test ids: TF-T<n> framework, FX-T<n> fixture builder, SIM-T<n> simulator.
// Original Shunt code.
#include "FixtureBuilder.h"
#include "Golden.h"
#include "TestFramework.h"
#include "shunt/net/Packets.h"
#include "shunt/net/Pcapng.h"
#include <atomic>
#include <fstream>
#include <sstream>
#include <unistd.h>

using namespace shunt::net;

namespace {

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string simPath() {
    if (const char* p = std::getenv("SHUNTSIM_PATH")) if (*p) return p;
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return "";
    std::string exe(buf, size_t(n));
    return exe.substr(0, exe.find_last_of('/')) + "/shuntsim";
}

// Runs shuntsim with args; stdout goes to outFile. Returns false when the simulator is unavailable
// (not built next to ShuntTests, or a foreign architecture without an emulator): callers skip.
bool runSim(const std::string& args, const std::string& outFile, int* status = nullptr) {
    static int available = -1;
    const std::string sim = simPath();
    std::string launcher;
    if (const char* l = std::getenv("SHUNT_SIM_LAUNCHER")) launcher = std::string(l) + " ";
    if (available < 0) {
        available = !sim.empty() && access(sim.c_str(), X_OK) == 0 &&
                    std::system((launcher + sim + " --help >/dev/null 2>&1").c_str()) == 0;
        if (!available) std::printf("  SKIP: shuntsim not runnable here (%s)\n", sim.c_str());
    }
    if (!available) return false;
    const int rc = std::system((launcher + sim + " " + args + " > " + outFile + " 2>&1").c_str());
    if (status) *status = rc;
    return true;
}

struct Rec { double t; uint16_t port; Event e; };

std::vector<Rec> decode(const std::string& pcap) {
    std::vector<Rec> out;
    const auto all = PcapngReader::readAll(pcap);
    if (all.empty()) return out;
    const int64_t t0 = all.front().timestampNs;
    for (auto& d : all) {
        auto ev = parseForPort(d.dstPort, d.payload.data(), d.payload.size(), d.timestampNs, d.srcIp);
        if (ev) out.push_back({double(d.timestampNs - t0) / 1e9, d.dstPort, *ev});
    }
    return out;
}

// Last player status of `device` at or before time t.
const PlayerStatusEvent* statusAt(const std::vector<Rec>& v, int device, double t) {
    const PlayerStatusEvent* r = nullptr;
    for (auto& x : v) {
        if (x.t > t) break;
        if (x.e.type == EventType::PlayerStatus && x.e.device == device) r = &x.e.status;
    }
    return r;
}

const OnAirEvent* onAirAt(const std::vector<Rec>& v, double t) {
    const OnAirEvent* r = nullptr;
    for (auto& x : v) {
        if (x.t > t) break;
        if (x.e.type == EventType::OnAir) r = &x.e.onAir;
    }
    return r;
}

// Record a scenario into `dir`; returns the capture path ("" when the simulator is unavailable).
std::string recordScenario(const tf::TempDir& dir, const std::string& name, const std::string& extra, const std::string& tag,
                           std::string* stdoutText = nullptr) {
    const std::string pcap = dir.file(name + tag + ".pcapng"), log = dir.file(name + tag + ".log");
    if (!runSim("--scenario " + name + " --time-scale 0 --verbose --record " + pcap + " " + extra, log)) return "";
    if (stdoutText) *stdoutText = slurp(log);
    return pcap;
}

} // namespace

// ---------------------------------------------------------------- TestFramework helpers

TEST_CASE("TF-T1 CHECK_STR, TEMP_DIR and WITH_TIMEOUT") {
    CHECK_STR(std::string("abc"), "abc");
    std::string kept;
    {
        auto dir = TEMP_DIR();
        kept = dir.path();
        CHECK(access(kept.c_str(), F_OK) == 0);
        std::ofstream(dir.file("x.txt")) << "hi";
        CHECK_STR(slurp(dir.file("x.txt")), "hi");
    }
    CHECK(access(kept.c_str(), F_OK) != 0);   // removed with its contents
    std::atomic<int> n{0};
    WITH_TIMEOUT(2000, {
        std::thread t([&] { n = 7; });
        t.join();
    });
    CHECK_EQ(n.load(), 7);
}

TEST_CASE("TF-T2 CHECK_JSON on Json values and text") {
    shunt::app::Json j = shunt::app::Json::object();
    j["name"] = "shunt";
    j["port"] = 8080;
    j["ratio"] = 0.5;
    j["on"] = true;
    shunt::app::Json list = shunt::app::Json::array();
    shunt::app::Json item = shunt::app::Json::object();
    item["id"] = 3;
    list.push(item);
    j["devices"] = list;
    shunt::app::Json feat = shunt::app::Json::object();
    feat["onair"] = shunt::app::Json::object();
    feat["onair"]["enabled"] = false;
    j["features"] = feat;
    CHECK_JSON(j, "name", "shunt");
    CHECK_JSON(j, "port", 8080);
    CHECK_JSON(j, "ratio", 0.5);
    CHECK_JSON(j, "on", true);
    CHECK_JSON(j, "devices[0].id", 3);
    CHECK_JSON(j, "features.onair.enabled", false);
    CHECK_JSON_TYPE(j, "devices", shunt::app::Json::Array);
    CHECK_JSON_TYPE(j, "features.onair", shunt::app::Json::Object);
    CHECK_JSON(j.dump(), "features.onair.enabled", false);          // JSON text is parsed
    CHECK_JSON("{\"a\":{\"b\":[1,2,{\"c\":\"x\"}]}}", "a.b[2].c", "x");
    CHECK(tf::jsonAt(j, "devices[1]") == nullptr);
    CHECK(tf::jsonAt(j, "nope.deeper") == nullptr);
    CHECK(tf::jsonAt(j, "name.x") == nullptr);
}

TEST_CASE("TF-T3 golden compare: diff, mismatch, update") {
    CHECK_STR(tf::unifiedDiff("a\nb\n", "a\nb\n"), "");
    const std::string d = tf::unifiedDiff("a\nb\nc\nd\n", "a\nb\nX\nd\n");
    CHECK(d.find("-c\n") != std::string::npos);
    CHECK(d.find("+X\n") != std::string::npos);
    CHECK(d.find("@@ -1,4 +1,4 @@") != std::string::npos);
    CHECK(d.find(" a\n") != std::string::npos);                       // context line
    const std::string ins = tf::unifiedDiff("a\nc\n", "a\nb\nc\n");
    CHECK(ins.find("+b\n") != std::string::npos && ins.find("-") == ins.find("--- "));
    // Two distant changes give two hunks.
    std::string big1, big2;
    for (int i = 0; i < 40; ++i) { big1 += "line" + std::to_string(i) + "\n"; big2 += (i == 2 || i == 35 ? "CHANGED\n" : "line" + std::to_string(i) + "\n"); }
    const std::string two = tf::unifiedDiff(big1, big2);
    size_t hunks = 0;
    for (size_t p = two.find("@@ "); p != std::string::npos; p = two.find("@@ ", p + 1)) if (two.compare(p, 4, "@@ -") == 0) ++hunks;
    CHECK_EQ(int(hunks), 2);

    auto dir = TEMP_DIR();
    std::string diff;
    CHECK(!tf::compareGolden(dir.path(), "g.txt", "one\n", false, &diff));       // missing
    CHECK(diff.find("missing") != std::string::npos);
    CHECK(tf::compareGolden(dir.path(), "g.txt", "one\n", true));                // UPDATE_GOLDEN writes
    CHECK_STR(slurp(dir.file("g.txt")), "one\n");
    CHECK(tf::compareGolden(dir.path(), "g.txt", "one\n", false, &diff));
    CHECK(!tf::compareGolden(dir.path(), "g.txt", "two\n", false, &diff));
    CHECK(diff.find("-one\n") != std::string::npos && diff.find("+two\n") != std::string::npos);
    CHECK_STR(slurp(dir.file("g.txt")), "one\n");                                 // not rewritten without update
}

// ---------------------------------------------------------------- FixtureBuilder

TEST_CASE("FX-T1 buildCapture is deterministic and parses back") {
    fixture::CaptureParams p;
    p.seed = 9; p.durationS = 6; p.jitterMs = 2; p.lossPct = 5;
    fixture::DeckSpec a; a.number = 1; a.master = true;
    fixture::DeckSpec b; b.number = 2; b.name = "CDJ-2000NXS2"; b.bpm = 126; b.onAir = false;
    p.decks = {a, b};
    p.changes.push_back({3.0, 2, [](fixture::DeckSpec& d) { d.onAir = true; }});
    p.changes.push_back({3.0, 1, [](fixture::DeckSpec& d) { d.master = false; }});
    p.changes.push_back({3.0, 2, [](fixture::DeckSpec& d) { d.master = true; }});
    const auto c1 = fixture::buildCapture(p), c2 = fixture::buildCapture(p);
    REQUIRE(c1.size() == c2.size());
    REQUIRE(c1.size() > 100);
    bool same = true;
    for (size_t i = 0; i < c1.size(); ++i) same = same && c1[i].payload == c2[i].payload && c1[i].timestampNs == c2[i].timestampNs && c1[i].srcIp == c2[i].srcIp;
    CHECK(same);
    p.seed = 10;
    const auto c3 = fixture::buildCapture(p);
    bool differs = c3.size() != c1.size();
    for (size_t i = 0; !differs && i < c1.size(); ++i) differs = c1[i].payload != c3[i].payload || c1[i].timestampNs != c3[i].timestampNs;
    CHECK(differs);

    auto dir = TEMP_DIR();
    REQUIRE(fixture::writeCapture(c1, dir.file("f.pcapng")));
    const auto back = decode(dir.file("f.pcapng"));
    CHECK_EQ(int(back.size()), int(c1.size()));
    int keepAlives = 0, beats = 0, statuses = 0, onAirs = 0, masterOn2 = 0;
    for (auto& r : back) {
        if (r.e.type == EventType::KeepAlive) ++keepAlives;
        if (r.e.type == EventType::Beat) ++beats;
        if (r.e.type == EventType::PlayerStatus) { ++statuses; if (r.t > 4 && r.e.device == 2 && r.e.status.master) ++masterOn2; }
        if (r.e.type == EventType::OnAir) ++onAirs;
    }
    CHECK(keepAlives >= 3 * 4);
    CHECK(beats > 20);
    CHECK(statuses >= 2 * 25);
    CHECK(onAirs >= 25);
    CHECK(masterOn2 > 5);
    const auto* s = statusAt(back, 2, 5.5);
    REQUIRE(s != nullptr);
    CHECK(s->onAir);
}

TEST_CASE("FX-T2 key bytes and track ids reach the status packet") {
    fixture::CaptureParams p;
    p.durationS = 2;
    fixture::DeckSpec a; a.number = 1; a.master = true; a.hasKey = true; a.key[0] = 0x8a; a.key[1] = 1; a.key[2] = 2; a.trackId = 4242;
    p.decks = {a};
    auto dir = TEMP_DIR();
    REQUIRE(fixture::writeCapture(p, dir.file("k.pcapng")));
    const auto v = decode(dir.file("k.pcapng"));
    const auto* s = statusAt(v, 1, 1.9);
    REQUIRE(s != nullptr);
    CHECK(s->hasKey);
    CHECK_EQ(int(s->key[0]), 0x8a);
    CHECK_EQ(int(s->key[2]), 2);
    CHECK_EQ(int(s->rekordboxId), 4242);
}

// ---------------------------------------------------------------- simulator scenarios

TEST_CASE("SIM-T1 every scenario is deterministic for a given seed") {
    const char* names[] = {"steady", "handoff", "nudge", "pitch", "hotcue", "loop", "pause", "loss", "jitter",
                           "onair", "onair-nomixer", "key", "phrase", "wrongbar", "longset"};
    auto dir = TEMP_DIR();
    for (const char* n : names) {
        const std::string extra = std::string("--seed 5") + (std::string(n) == "longset" ? " --duration 340" : "");
        std::string out1, out2;
        const std::string p1 = recordScenario(dir, n, extra, "-a", &out1);
        if (p1.empty()) return;   // simulator unavailable: skipped
        const std::string p2 = recordScenario(dir, n, extra, "-b", &out2);
        INFO("scenario %s\n", n);
        CHECK(slurp(p1).size() > 1000);
        CHECK(slurp(p1) == slurp(p2));
        CHECK(out1 == out2);
        CHECK(out1.find("scenario " + std::string(n) + " t=") != std::string::npos || std::string(n) == "steady" ||
              std::string(n) == "loss" || std::string(n) == "jitter");
    }
}

TEST_CASE("SIM-T2 seed changes jitter and loss, not the structure") {
    auto dir = TEMP_DIR();
    const std::string a = recordScenario(dir, "jitter", "--seed 1", "-s1");
    if (a.empty()) return;
    const std::string b = recordScenario(dir, "jitter", "--seed 2", "-s2");
    CHECK(slurp(a) != slurp(b));
    const std::string c = recordScenario(dir, "steady", "--seed 1", "-s1");
    const std::string d = recordScenario(dir, "steady", "--seed 2", "-s2");
    CHECK(slurp(c) != slurp(d));      // seed also sets the initial beat phase
}

TEST_CASE("SIM-T3 --verbose prints timestamped scenario events (golden)") {
    auto dir = TEMP_DIR();
    std::string out;
    if (recordScenario(dir, "onair", "", "", &out).empty()) return;
    std::string events;
    for (const auto& line : tf::splitLines(out)) if (line.rfind("scenario ", 0) == 0) events += line + "\n";
    CHECK_GOLDEN("shuntsim-onair-verbose.txt", events);
}

TEST_CASE("SIM-T4 onair: mixer packets, crossfade, all off, return") {
    auto dir = TEMP_DIR();
    const std::string pcap = recordScenario(dir, "onair", "", "");
    if (pcap.empty()) return;
    const auto v = decode(pcap);
    const auto* o5 = onAirAt(v, 5);
    const auto* o11 = onAirAt(v, 11);
    const auto* o31 = onAirAt(v, 31);
    const auto* o40 = onAirAt(v, 40);
    REQUIRE(o5 && o11 && o31 && o40);
    CHECK(o5->channels[0] == 1 && o5->channels[1] == 0);
    CHECK(o11->channels[0] == 0 && o11->channels[1] == 1);
    for (int i = 0; i < 4; ++i) CHECK(o31->channels[i] == 0);
    CHECK(o40->channels[1] == 1 && o40->channels[0] == 0);
    CHECK(statusAt(v, 1, 5)->master && !statusAt(v, 2, 5)->master);
    CHECK(!statusAt(v, 1, 13)->master && statusAt(v, 2, 13)->master);
    CHECK(statusAt(v, 1, 5)->onAir && !statusAt(v, 2, 5)->onAir);
    CHECK(!statusAt(v, 1, 11)->onAir && statusAt(v, 2, 11)->onAir);
    CHECK(v.back().t > 44.0 && v.back().t < 45.0);
}

TEST_CASE("SIM-T5 onair-nomixer: no 0x03 packets, F-byte bits carry on-air") {
    auto dir = TEMP_DIR();
    const std::string pcap = recordScenario(dir, "onair-nomixer", "", "");
    if (pcap.empty()) return;
    const auto v = decode(pcap);
    int onAirPackets = 0;
    for (auto& r : v) if (r.e.type == EventType::OnAir) ++onAirPackets;
    CHECK_EQ(onAirPackets, 0);
    CHECK(statusAt(v, 1, 5)->onAir && !statusAt(v, 2, 5)->onAir);
    CHECK(!statusAt(v, 1, 11)->onAir && statusAt(v, 2, 11)->onAir);
    CHECK(!statusAt(v, 1, 31)->onAir && !statusAt(v, 2, 31)->onAir);
    CHECK(statusAt(v, 2, 40)->onAir);
}

TEST_CASE("SIM-T6 key: track loads and --key-bytes") {
    auto dir = TEMP_DIR();
    const std::string pcap = recordScenario(dir, "key", "--key-bytes 8a,01,02", "");
    if (pcap.empty()) return;
    const auto v = decode(pcap);
    const auto* d1 = statusAt(v, 1, 5);
    const auto* d2a = statusAt(v, 2, 5);
    const auto* d2b = statusAt(v, 2, 25);
    REQUIRE(d1 && d2a && d2b);
    CHECK_EQ(int(d1->rekordboxId), 2001);
    CHECK_EQ(int(d2a->rekordboxId), 2002);
    CHECK_EQ(int(d2b->rekordboxId), 2003);
    CHECK(d1->hasKey && d1->key[0] == 0x8a && d1->key[1] == 1 && d1->key[2] == 2);
    CHECK(d2b->hasKey && d2b->key[0] == 0x8a && d2b->key[2] == 2);
    CHECK(d1->onAir && d1->playing);
    CHECK(!d2a->onAir && !d2a->playing);
    CHECK_EQ(int(d1->packetLength), 0x200);
    // Placeholder keys differ per track when --key-bytes is absent.
    const std::string pcap2 = recordScenario(dir, "key", "", "-default");
    const auto w = decode(pcap2);
    CHECK(statusAt(w, 1, 5)->key[0] != statusAt(w, 2, 5)->key[0]);
    CHECK(statusAt(w, 2, 5)->key[0] != statusAt(w, 2, 25)->key[0]);
}

TEST_CASE("SIM-T7 phrase: beat numbers advance, jump back 32, loop 4") {
    auto dir = TEMP_DIR();
    const std::string pcap = recordScenario(dir, "phrase", "", "");
    if (pcap.empty()) return;
    const auto v = decode(pcap);
    const auto* a = statusAt(v, 1, 10);
    const auto* b = statusAt(v, 1, 19.5);
    const auto* c = statusAt(v, 1, 21);
    REQUIRE(a && b && c);
    CHECK(a->beatNumber >= 15 && a->beatNumber < b->beatNumber);
    CHECK(int(c->beatNumber) < int(b->beatNumber) - 25);
    CHECK_EQ(int((c->beatNumber - 1) % 4 + 1), int(c->beatInBar));
    std::vector<uint32_t> loopBeats;
    for (auto& r : v)
        if (r.t > 41 && r.t < 47 && r.e.type == EventType::PlayerStatus && r.e.device == 1) loopBeats.push_back(r.e.status.beatNumber);
    uint32_t lo = 0xffffffff, hi = 0;
    for (auto n : loopBeats) { lo = std::min(lo, n); hi = std::max(hi, n); }
    CHECK(!loopBeats.empty());
    CHECK(hi - lo <= 3);
    CHECK(statusAt(v, 1, 55)->beatNumber > 0);
}

TEST_CASE("SIM-T8 wrongbar: reported beatInBar is shifted relative to the true bar") {
    auto dir = TEMP_DIR();
    for (int shift : {2, 1}) {
        const std::string pcap = recordScenario(dir, "wrongbar", shift == 2 ? "" : "--bar-shift 1", "-" + std::to_string(shift));
        if (pcap.empty()) return;
        const auto v = decode(pcap);
        int checked = 0, bad = 0;
        for (auto& r : v) {
            if (r.e.type != EventType::PlayerStatus || r.e.device != 1) continue;
            const int trueBar = int((r.e.status.beatNumber - 1) % 4);
            ++checked;
            if (int(r.e.status.beatInBar) - 1 != (trueBar + shift) % 4) ++bad;
        }
        CHECK(checked > 100);
        CHECK_EQ(bad, 0);
    }
}

TEST_CASE("SIM-T9 longset: handoffs, six tracks, loss and jitter apply") {
    auto dir = TEMP_DIR();
    std::string out;
    const std::string pcap = recordScenario(dir, "longset", "--duration 700", "", &out);
    if (pcap.empty()) return;
    CHECK(out.find("t=300.400 deck 2 master") != std::string::npos);
    CHECK(out.find("t=600.400 deck 1 master") != std::string::npos);
    CHECK(out.find("loads track 3003") != std::string::npos);
    CHECK(out.find("loads track 3004") != std::string::npos);
    const auto v = decode(pcap);
    CHECK(statusAt(v, 1, 290)->master && !statusAt(v, 2, 290)->master);
    CHECK(statusAt(v, 2, 310)->master && !statusAt(v, 1, 310)->master);
    CHECK(statusAt(v, 1, 650)->master);
    CHECK_EQ(int(statusAt(v, 1, 400)->rekordboxId), 3003);
    CHECK_EQ(int(statusAt(v, 2, 650)->rekordboxId), 3004);
    // 1 percent loss: deck 1 beats over its first 280 s fall a little short of the ideal count.
    int beats1 = 0;
    for (auto& r : v) if (r.t < 280 && r.e.type == EventType::Beat && r.e.device == 1) ++beats1;
    const double ideal = 280 * 128.0 / 60.0;
    CHECK(beats1 < ideal - 3 && beats1 > ideal * 0.95);
}

TEST_CASE("SIM-T10 unknown scenario and bad flags fail cleanly") {
    auto dir = TEMP_DIR();
    int rc = 0;
    if (!runSim("--scenario nope --record " + dir.file("x.pcapng"), dir.file("o.txt"), &rc)) return;
    CHECK(rc != 0);
    CHECK(slurp(dir.file("o.txt")).find("unknown scenario") != std::string::npos);
    runSim("--scenario key --key-bytes zz --record " + dir.file("x.pcapng"), dir.file("o2.txt"), &rc);
    CHECK(rc != 0);
    runSim("--scenario steady", dir.file("o3.txt"), &rc);     // neither --dest nor --record
    CHECK(rc != 0);
}
