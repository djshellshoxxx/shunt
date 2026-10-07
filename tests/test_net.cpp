// ES-01 tests N-T3 to N-T6 plus device table, pcapng and SPSC queue checks.
#include "TestFramework.h"
#include "shunt/net/NetworkStack.h"
#include <cstring>
#include <cstdio>
#include <deque>

using namespace shunt::net;

namespace {

struct FakeClock : IClock {
    int64_t now = 1'000'000'000;
    int64_t nowNs() override { return now; }
    void advanceMs(int64_t ms) { now += ms * 1'000'000; }
};

struct SentPacket { uint16_t port; uint32_t ip; std::vector<uint8_t> bytes; };

struct MockSender : ISender {
    std::vector<SentPacket> sent;
    bool send(uint16_t dstPort, uint32_t dstIp, const uint8_t* data, size_t len) override {
        sent.push_back({dstPort, dstIp, std::vector<uint8_t>(data, data + len)});
        return true;
    }
    size_t countType(uint8_t t) const { size_t n = 0; for (auto& p : sent) if (p.bytes[0x0a] == t) ++n; return n; }
};

const std::array<uint8_t, 6> kOurMac{0x02, 0, 0, 0, 0, 1};

Event keepAliveFrom(uint8_t number, const char* name, DeviceKind kind, int64_t now) {
    uint8_t buf[64];
    KeepAliveParams p;
    p.name = name; p.kind = kind; p.number = number; p.mac = {0, 0xe0, 0x36, 0, 0, number}; p.ip = 0xc0a80100u + number;
    const size_t n = buildKeepAlive(buf, p);
    return *parseAnnounce(buf, n, now, p.ip);
}

Event defendFor(uint8_t number, int64_t now) {
    uint8_t buf[64];
    const size_t n = buildDefend(buf, "CDJ-2000NXS2", number);
    return *parseAnnounce(buf, n, now);
}

// Mock socket layer: scripted inbound datagrams, every outbound send recorded.
struct MockSocket;
struct MockFactory : ISocketFactory {
    std::deque<Datagram> inbound;
    std::vector<SentPacket> sent;
    int64_t now = 1'000'000'000;
    std::unique_ptr<IUdpSocket> create() override;
    bool waitAny(const std::vector<IUdpSocket*>&, int) override { return !inbound.empty(); }
    int64_t nowNs() override { return now; }
};
struct MockSocket : IUdpSocket {
    MockFactory& f; uint16_t port_ = 0;
    explicit MockSocket(MockFactory& fac) : f(fac) {}
    bool bind(uint16_t p) override { port_ = p; return true; }
    bool receive(Datagram& out) override {
        for (auto it = f.inbound.begin(); it != f.inbound.end(); ++it)
            if (it->dstPort == port_) { out = *it; f.inbound.erase(it); return true; }
        return false;
    }
    bool send(const uint8_t* d, size_t n, uint32_t ip, uint16_t p) override { f.sent.push_back({p, ip, {d, d + n}}); return true; }
    uint16_t port() const override { return port_; }
    const char* timestampSource() const override { return "mock"; }
};
std::unique_ptr<IUdpSocket> MockFactory::create() { return std::make_unique<MockSocket>(*this); }

void inject(MockFactory& f, uint16_t port, const uint8_t* d, size_t n, uint32_t srcIp) {
    Datagram g;
    g.dstPort = port; g.srcIp = srcIp; g.srcPort = port; g.recvNs = f.now; g.len = n;
    std::memcpy(g.data, d, n);
    f.inbound.push_back(g);
}

} // namespace

// ---------------------------------------------------------------- N-T3

TEST_CASE("N-T3 Follow mode with 1..4 taken ends on 7") {
    FakeClock clock; MockSender sender; DeviceTable devices;
    ClaimMachine::Config cfg; cfg.mode = Mode::Follow; cfg.mac = kOurMac; cfg.ip = 0xc0a80164;
    ClaimMachine cm(cfg, sender, clock, devices);
    cm.start();
    CHECK(cm.state() == ClaimMachine::State::Watching);
    for (uint8_t n = 1; n <= 4; ++n) devices.onKeepAlive(keepAliveFrom(n, "CDJ-2000NXS2", DeviceKind::CDJ, clock.now));
    for (int i = 0; i < 200 && cm.state() != ClaimMachine::State::Active; ++i) { clock.advanceMs(100); cm.tick(); }
    CHECK(cm.state() == ClaimMachine::State::Active);
    CHECK_EQ(cm.number(), 7);
    CHECK_EQ(sender.countType(0x0a), 3u);   // hello x3
    CHECK_EQ(sender.countType(0x00), 3u);   // stage 1 x3
    CHECK_EQ(sender.countType(0x02), 3u);   // stage 2 x3
    CHECK_EQ(sender.countType(0x04), 3u);   // stage 3 x3
    CHECK_EQ(sender.countType(0x06), 1u);   // first keep-alive on Active
    // keep-alive cadence 1500 ms
    const size_t before = sender.sent.size();
    for (int i = 0; i < 30; ++i) { clock.advanceMs(100); cm.tick(); }
    CHECK_EQ(sender.sent.size() - before, 2u);
    // defend our number when another device claims it
    uint8_t buf[64];
    size_t n = buildClaimStage2(buf, "CDJ-2000NXS2", 0xc0a80199, {1, 2, 3, 4, 5, 6}, 7, 1, 0x01);
    cm.onAnnounce(*parseAnnounce(buf, n, clock.now));
    CHECK_EQ(sender.countType(0x08), 1u);
}

TEST_CASE("N-T3 Follow mode with 7 defended ends on 8") {
    FakeClock clock; MockSender sender; DeviceTable devices;
    ClaimMachine::Config cfg; cfg.mode = Mode::Follow; cfg.mac = kOurMac; cfg.ip = 0xc0a80164;
    ClaimMachine cm(cfg, sender, clock, devices);
    cm.start();
    for (uint8_t n = 1; n <= 4; ++n) devices.onKeepAlive(keepAliveFrom(n, "CDJ-2000NXS2", DeviceKind::CDJ, clock.now));
    bool defended = false;
    for (int i = 0; i < 300 && cm.state() != ClaimMachine::State::Active; ++i) {
        clock.advanceMs(100); cm.tick();
        if (!defended && cm.state() == ClaimMachine::State::Claiming && cm.stage() == ClaimMachine::Stage::Stage2) {
            cm.onAnnounce(defendFor(7, clock.now));
            defended = true;
        }
    }
    CHECK(defended);
    CHECK(cm.state() == ClaimMachine::State::Active);
    CHECK_EQ(cm.number(), 8);
}

TEST_CASE("N-T3 Lead mode on a taken number fails without further claims") {
    FakeClock clock; MockSender sender; DeviceTable devices;
    ClaimMachine::Config cfg; cfg.mode = Mode::Lead; cfg.leadNumber = 2; cfg.mac = kOurMac; cfg.ip = 0xc0a80164;
    {
        ClaimMachine cm(cfg, sender, clock, devices);
        cm.start();
        for (int i = 0; i < 100 && cm.state() != ClaimMachine::State::Failed; ++i) {
            clock.advanceMs(100); cm.tick();
            if (cm.state() == ClaimMachine::State::Claiming) cm.onAnnounce(defendFor(2, clock.now));
        }
        CHECK(cm.state() == ClaimMachine::State::Failed);
        CHECK(cm.failure() == "number in use");
        const size_t after = sender.sent.size();
        for (int i = 0; i < 50; ++i) { clock.advanceMs(100); cm.tick(); }
        CHECK_EQ(sender.sent.size(), after);
        CHECK_EQ(sender.countType(0x06), 0u);
    }
    // Number already visible during Watching: fail before sending anything.
    MockSender sender2;
    ClaimMachine cm2(cfg, sender2, clock, devices);
    cm2.start();
    devices.onKeepAlive(keepAliveFrom(2, "CDJ-2000NXS2", DeviceKind::CDJ, clock.now));
    for (int i = 0; i < 100; ++i) { clock.advanceMs(100); cm2.tick(); }
    CHECK(cm2.state() == ClaimMachine::State::Failed);
    CHECK_EQ(sender2.sent.size(), 0u);
}

TEST_CASE("N-T3 mixer assignment 0x05 ends the claim early") {
    FakeClock clock; MockSender sender; DeviceTable devices;
    ClaimMachine::Config cfg; cfg.mode = Mode::Follow; cfg.mac = kOurMac; cfg.ip = 0xc0a80164;
    ClaimMachine cm(cfg, sender, clock, devices);
    cm.start();
    for (int i = 0; i < 45; ++i) { clock.advanceMs(100); cm.tick(); }
    CHECK(cm.state() == ClaimMachine::State::Claiming);
    Event e; e.type = EventType::Claim; e.claim.packetType = 0x05; e.claim.number = 7;
    cm.onAnnounce(e);
    CHECK(cm.state() == ClaimMachine::State::Active);
}

// ---------------------------------------------------------------- N-T4

TEST_CASE("N-T4 CDJ-3000 keep-alive golden bytes") {
    FakeClock clock; MockSender sender; DeviceTable devices;
    ClaimMachine::Config cfg; cfg.mode = Mode::Follow; cfg.mac = kOurMac; cfg.ip = 0xc0a80164;
    ClaimMachine cm(cfg, sender, clock, devices);
    cm.start();
    for (int i = 0; i < 200 && cm.state() != ClaimMachine::State::Active; ++i) { clock.advanceMs(100); cm.tick(); }
    REQUIRE(cm.state() == ClaimMachine::State::Active);
    const SentPacket* ka = nullptr;
    for (auto& p : sender.sent) if (p.bytes[0x0a] == 0x06) { ka = &p; break; }
    REQUIRE(ka != nullptr);
    CHECK_EQ(ka->port, kAnnouncePort);
    const uint8_t golden[54] = {
        0x51, 0x73, 0x70, 0x74, 0x31, 0x57, 0x6d, 0x4a, 0x4f, 0x4c, 0x06, 0x00,
        'S', 'h', 'u', 'n', 't', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0x01, 0x01, 0x00, 0x36, 0x07, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01,
        0xc0, 0xa8, 0x01, 0x64, 0x01, 0x00, 0x00, 0x00, 0x01, 0x64};
    CHECK_EQ(ka->bytes.size(), 54u);
    CHECK(std::memcmp(ka->bytes.data(), golden, 54) == 0);
    CHECK_EQ(ka->bytes[0x35], 0x64);
    // hello is the CDJ-3000-compatible form
    for (auto& p : sender.sent) if (p.bytes[0x0a] == 0x0a) CHECK_EQ(p.bytes.size(), kHelloLen3000);
}

// ---------------------------------------------------------------- N-T5

TEST_CASE("N-T5 master handoff produces exactly one MasterChanged within 200 ms") {
    MasterTracker mt;
    uint8_t buf[512];
    auto status = [&](uint8_t n, bool master, uint8_t handoffTo, int64_t t) {
        CdjStatusParams p; p.number = n; p.flags = kFlagPlay | (master ? kFlagMaster : 0); p.masterMeaning = master ? 2 : 0;
        p.handoffTo = handoffTo;
        size_t len = buildCdjStatus(buf, p);
        Event e = *parseStatusPort(buf, len, t);
        return mt.onEvent(e);
    };
    int64_t t = 0;
    const int64_t step = 200'000'000;   // 200 ms status period
    std::vector<MasterChangedEvent> changes;
    auto collect = [&](std::optional<Event> e) { if (e) changes.push_back(e->masterChanged); };
    // steady: deck 1 master, deck 2 not
    for (int i = 0; i < 10; ++i) { collect(status(1, true, 0xff, t)); collect(status(2, false, 0xff, t)); t += step; }
    REQUIRE(changes.size() == 1u);
    CHECK_EQ(changes[0].from, 0); CHECK_EQ(changes[0].to, 1);
    CHECK(mt.state() == MasterTracker::State::Master);
    changes.clear();
    // deck 2 requests master; deck 1 announces handoff (Mh = 2); deck 2 asserts on the next status
    collect(status(1, true, 2, t));
    CHECK(mt.state() == MasterTracker::State::Handoff);
    collect(status(2, false, 0xff, t)); t += step;
    const int64_t assertedAt = t;
    collect(status(1, false, 0xff, t));
    collect(status(2, true, 0xff, t));
    REQUIRE(changes.size() == 1u);
    CHECK_EQ(changes[0].from, 1); CHECK_EQ(changes[0].to, 2);
    CHECK(mt.state() == MasterTracker::State::Master);
    CHECK_EQ(mt.master(), 2);
    for (int i = 0; i < 10; ++i) { t += step; collect(status(1, false, 0xff, t)); collect(status(2, true, 0xff, t)); collect(mt.tick(t)); }
    CHECK_EQ(changes.size(), 1u);
    CHECK(t - assertedAt >= 0);
    // beats are tagged for the current master
    BeatParams bp; bp.number = 2; bp.bpm100 = 13000;
    size_t n = buildBeat(buf, bp);
    Event b = *parseBeatPort(buf, n, t);
    mt.onEvent(b);
    CHECK(b.beat.isMaster);
    bp.number = 1; n = buildBeat(buf, bp);
    b = *parseBeatPort(buf, n, t);
    mt.onEvent(b);
    CHECK(!b.beat.isMaster);
    // silence from the master for 2 s -> NoMaster (no mixer)
    collect(mt.tick(t + 2'100'000'000LL));
    CHECK(mt.state() == MasterTracker::State::NoMaster);
    CHECK_EQ(changes.back().to, 0);
}

// ---------------------------------------------------------------- N-T6

TEST_CASE("N-T6 Passive mode never sends a datagram") {
    MockFactory f;
    StackConfig cfg; cfg.mode = Mode::Passive; cfg.address = 0x7f000001; cfg.netmask = 0xff000000; cfg.mac = kOurMac;
    NetworkStack stack(f, cfg);
    REQUIRE(stack.start());
    uint8_t buf[512];
    for (int i = 0; i < 20; ++i) {
        KeepAliveParams k; k.name = "CDJ-2000NXS2"; k.number = 1; k.mac = {0, 0xe0, 0x36, 1, 1, 1}; k.ip = 0x7f000002;
        inject(f, kAnnouncePort, buf, buildKeepAlive(buf, k), 0x7f000002);
        BeatParams b; b.number = 1; b.bpm100 = 12800;
        inject(f, kBeatPort, buf, buildBeat(buf, b), 0x7f000002);
        CdjStatusParams s; s.number = 1; s.flags = kFlagPlay | kFlagMaster;
        inject(f, kStatusPort, buf, buildCdjStatus(buf, s), 0x7f000002);
        size_t n = buildClaimStage2(buf, "CDJ", 0x7f000003, {9, 9, 9, 9, 9, 9}, 7, 1, 1);
        inject(f, kAnnouncePort, buf, n, 0x7f000003);
        f.now += 500'000'000;
        stack.poll(10);
    }
    CHECK_EQ(f.sent.size(), 0u);
    CHECK_EQ(stack.counters().sent, 0u);
    CHECK(stack.claim().state() == ClaimMachine::State::Idle);
    // Passive mode binds only 50001: beats arrive, unicast status does not.
    Event e; size_t beats = 0, statuses = 0;
    while (stack.pop(e)) { if (e.type == EventType::Beat) ++beats; if (e.type == EventType::PlayerStatus) ++statuses; }
    CHECK_EQ(beats, 20u);
    CHECK_EQ(statuses, 0u);
    // Direct send attempts are refused too.
    CHECK(!stack.send(kAnnouncePort, 0xffffffff, buf, 54));
    CHECK_EQ(f.sent.size(), 0u);
}

TEST_CASE("Follow mode stack claims a number and tracks devices through the sockets") {
    MockFactory f;
    StackConfig cfg; cfg.mode = Mode::Follow; cfg.address = 0xc0a80164; cfg.netmask = 0xffffff00; cfg.broadcast = 0xc0a801ff; cfg.mac = kOurMac;
    NetworkStack stack(f, cfg);
    REQUIRE(stack.start());
    uint8_t buf[512];
    for (int i = 0; i < 80; ++i) {
        for (uint8_t n = 1; n <= 2; ++n) {
            KeepAliveParams k; k.name = "CDJ-3000"; k.number = n; k.mac = {0, 0xe0, 0x36, 1, 1, n}; k.ip = 0xc0a80100u + n;
            inject(f, kAnnouncePort, buf, buildKeepAlive(buf, k), k.ip);
        }
        KeepAliveParams m; m.name = "DJM-900NXS2"; m.kind = DeviceKind::Mixer; m.number = 0x21; m.mac = {0, 0xe0, 0x36, 2, 2, 2}; m.ip = 0xc0a80121;
        inject(f, kAnnouncePort, buf, buildKeepAlive(buf, m), m.ip);
        // off-subnet datagram is filtered
        inject(f, kAnnouncePort, buf, buildKeepAlive(buf, m), 0x0a000001);
        f.now += 100'000'000;
        stack.poll(10);
    }
    CHECK(stack.claim().state() == ClaimMachine::State::Active);
    CHECK_EQ(stack.claim().number(), 7);
    CHECK_EQ(stack.devices().size(), 3u);
    CHECK(stack.devices().mixerPresent());
    CHECK(stack.devices().find(1)->model == Model::CDJ3000);
    CHECK(stack.counters().filtered > 0);
    CHECK(f.sent.size() > 0);
    for (auto& p : f.sent) { CHECK_EQ(p.port, kAnnouncePort); CHECK_EQ(p.ip, 0xc0a801ffu); }
    // expiry after 10 s of silence
    f.now += 11'000'000'000LL;
    stack.poll(0);
    CHECK_EQ(stack.devices().size(), 0u);
}

// ---------------------------------------------------------------- device table / model inference

TEST_CASE("device table model inference and capabilities") {
    CHECK(inferModel("CDJ-3000", 0, DeviceKind::CDJ) == Model::CDJ3000);
    CHECK(inferModel("CDJ-2000NXS2", 0, DeviceKind::CDJ) == Model::NXS2);
    CHECK(inferModel("CDJ-TOUR1", 0, DeviceKind::CDJ) == Model::NXS2);
    CHECK(inferModel("CDJ-2000nexus", 0, DeviceKind::CDJ) == Model::Nexus);
    CHECK(inferModel("CDJ-2000", 0, DeviceKind::CDJ) == Model::PreNexus);
    CHECK(inferModel("XDJ-1000MK2", 0, DeviceKind::CDJ) == Model::XDJ);
    CHECK(inferModel("XDJ-XZ", 0, DeviceKind::CDJ) == Model::XZ);
    CHECK(inferModel("XDJ-AZ", 0, DeviceKind::CDJ) == Model::AZ);
    CHECK(inferModel("OPUS-QUAD", 0, DeviceKind::CDJ) == Model::Opus);
    CHECK(inferModel("DJM-900NXS2", 0, DeviceKind::Mixer) == Model::Mixer);
    CHECK(inferModel("rekordbox", 0, DeviceKind::Rekordbox) == Model::Rekordbox);
    CHECK(inferModel("MYSTERY", 0x200, DeviceKind::CDJ) == Model::CDJ3000);
    CHECK(inferModel("MYSTERY", 0xd4, DeviceKind::CDJ) == Model::Nexus);
    CHECK(inferModel("MYSTERY", 0, DeviceKind::CDJ) == Model::Unknown);
    CHECK(!capabilitiesFor(Model::Unknown).verified);
    CHECK(!capabilitiesFor(Model::Opus).sendsBeats);
    CHECK(capabilitiesFor(Model::Opus).opusQuadQuirks);
    CHECK(capabilitiesFor(Model::CDJ3000).sendsPrecisePosition);
    CHECK(capabilitiesFor(Model::CDJ3000).supportsNumbers5and6);
    CHECK(capabilitiesFor(Model::AZ).supportsNumbers5and6);
    CHECK(!capabilitiesFor(Model::PreNexus).hasFByte);

    DeviceTable t;
    auto ev = t.onKeepAlive(keepAliveFrom(5, "MYSTERY", DeviceKind::CDJ, 0));
    REQUIRE(ev.has_value());
    CHECK(ev->deviceEvent.kind == DeviceEvent::Joined);
    CHECK(t.find(5)->model == Model::Unknown);
    uint8_t buf[512];
    CdjStatusParams s; s.number = 5; s.length = 0xd4;
    Event st = *parseStatusPort(buf, buildCdjStatus(buf, s), 0);
    ev = t.onStatus(st);
    REQUIRE(ev.has_value());
    CHECK(ev->deviceEvent.kind == DeviceEvent::ModelChanged);
    CHECK(t.find(5)->model == Model::Nexus);
    CHECK(!t.onKeepAlive(keepAliveFrom(5, "MYSTERY", DeviceKind::CDJ, 1)).has_value());
}

TEST_CASE("pcapng write and read round trip") {
    const std::string path = "/tmp/shunt_test_roundtrip.pcapng";
    {
        PcapngWriter w;
        REQUIRE(w.open(path));
        uint8_t buf[512];
        BeatParams b; b.number = 1; b.bpm100 = 12800;
        CapturedDatagram d;
        d.timestampNs = 1'700'000'000'123'456'789LL; d.srcIp = 0xc0a80101; d.dstIp = 0xc0a801ff; d.srcPort = 50001; d.dstPort = 50001;
        d.payload.assign(buf, buf + buildBeat(buf, b));
        CHECK(w.write(d));
        d.timestampNs += 468'750'000; d.payload[0x5c] = 2;
        CHECK(w.write(d));
    }
    auto all = PcapngReader::readAll(path);
    REQUIRE(all.size() == 2u);
    CHECK_EQ(all[0].timestampNs, 1'700'000'000'123'456'789LL);
    CHECK_EQ(all[0].srcIp, 0xc0a80101u);
    CHECK_EQ(all[0].dstPort, 50001);
    CHECK_EQ(all[0].payload.size(), kBeatLen);
    auto ev = parseBeatPort(all[1].payload.data(), all[1].payload.size(), 0);
    REQUIRE(ev.has_value());
    CHECK_EQ(ev->beat.beatInBar, 2);
    std::remove(path.c_str());
}

TEST_CASE("SPSC queue") {
    SpscQueue<int, 8> q;
    for (int i = 0; i < 8; ++i) CHECK(q.push(i));
    CHECK(!q.push(99));
    CHECK_EQ(q.dropped(), 1u);
    int v = -1;
    for (int i = 0; i < 8; ++i) { CHECK(q.pop(v)); CHECK_EQ(v, i); }
    CHECK(!q.pop(v));
}

TEST_CASE("N-T1 parser fuzz never crashes or yields out-of-range fields") {
    uint32_t seed = 12345;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return seed; };
    uint8_t buf[600];
    for (int i = 0; i < 20000; ++i) {
        const size_t len = rnd() % sizeof buf;
        for (size_t j = 0; j < len; ++j) buf[j] = uint8_t(rnd());
        if (rnd() & 1) std::memcpy(buf, kMagic, 10);
        for (uint16_t port : {kAnnouncePort, kBeatPort, kStatusPort}) {
            auto ev = parseForPort(port, buf, len, 0);
            if (!ev) continue;
            if (ev->type == EventType::Beat) CHECK(ev->beat.beatInBar <= 4);
            if (ev->type == EventType::PlayerStatus) CHECK(ev->status.beatInBar <= 4);
            if (ev->type == EventType::KeepAlive) CHECK(uint8_t(ev->keepAlive.kind) <= 3);
        }
    }
}
