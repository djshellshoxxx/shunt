// FS-04 tests ML-T1..ML-T7: MIDI parser (plus fuzz), trigger edge, absolute mapping, learn model,
// persistence, FIFO/socketpair end to end, API shapes. Original Shunt code.
#include "TestFramework.h"
#include "../../app/src/features/MidiLearn.h"
#include "shunt/app/Api.h"
#include "shunt/out/MidiIn.h"
#include <atomic>
#include <cstdint>
#include <fcntl.h>
#include <random>
#include <sys/socket.h>
#include <sys/stat.h>
#include <csignal>
#include <unistd.h>

using namespace shunt::app;
using shunt::out::MidiMessage;
using shunt::out::MidiParser;

namespace {
std::vector<MidiMessage> parseAll(const std::vector<uint8_t>& bytes) {
    MidiParser p;
    std::vector<MidiMessage> v;
    MidiMessage m;
    for (uint8_t b : bytes) if (p.feed(b, m)) v.push_back(m);
    return v;
}
MidiMessage msg(MidiMessage::Kind k, int ch, int n, int v) {
    MidiMessage m;
    m.kind = k; m.channel = uint8_t(ch); m.number = uint8_t(n); m.value = uint8_t(v);
    return m;
}
MidiBinding bind(const std::string& a, int ch, MidiBinding::Type t, int n, MidiBinding::Mode mode = MidiBinding::Trigger) {
    MidiBinding b;
    b.action = a; b.channel = ch; b.type = t; b.number = n; b.mode = mode;
    return b;
}
HttpRequest req(const std::string& method, const std::string& path, const std::string& body = "") {
    HttpRequest r;
    r.method = method; r.path = path; r.body = body;
    return r;
}
template <class F> bool waitFor(F cond, int ms = 3000) {
    for (int i = 0; i < ms / 2; ++i) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return cond();
}
Settings offlineSettings() {
    Settings s;
    s.interfaceName = "fs04-no-such-interface";
    return s;
}
Json parseJson(const std::string& t) { Json j; Json::parse(t, j); return j; }
std::string patch(const std::string& port, const std::string& bindings, bool enabled = true) {
    return std::string("{\"features\":{\"midilearn\":{\"enabled\":") + (enabled ? "true" : "false") + ",\"port\":\"" + port + "\",\"bindings\":" + bindings + "}}}";
}
}

TEST_CASE("ML-T1 parser: running status, real-time interleave, truncated, SysEx skip") {
    // running status: one status byte, three messages
    auto v = parseAll({0x90, 60, 100, 62, 101, 64, 102});
    REQUIRE(v.size() == 3);
    CHECK(v[1].kind == MidiMessage::NoteOn && v[1].number == 62 && v[1].value == 101 && v[1].channel == 0);
    // note on velocity 0 == note off, also under running status
    v = parseAll({0x93, 60, 100, 60, 0});
    REQUIRE(v.size() == 2);
    CHECK(v[1].kind == MidiMessage::NoteOff && v[1].channel == 3 && v[1].value == 0);
    // real-time bytes anywhere, including between data bytes, are transparent
    v = parseAll({0xF8, 0xB1, 0xF8, 7, 0xFE, 100, 0xFF, 0xF8, 7, 0xF8, 90});
    REQUIRE(v.size() == 2);
    CHECK(v[0].kind == MidiMessage::ControlChange && v[0].channel == 1 && v[0].number == 7 && v[0].value == 100);
    CHECK(v[1].value == 90);
    // one-data-byte messages
    v = parseAll({0xC5, 12, 0xD2, 40, 13});
    REQUIRE(v.size() == 3);
    CHECK(v[0].kind == MidiMessage::ProgramChange && v[0].channel == 5 && v[0].number == 12);
    CHECK(v[1].kind == MidiMessage::ChannelPressure && v[2].number == 13);
    // truncated: a new status byte drops the pending message
    v = parseAll({0x90, 60, 0x80, 61, 5});
    REQUIRE(v.size() == 1);
    CHECK(v[0].kind == MidiMessage::NoteOff && v[0].number == 61 && v[0].value == 5);
    // SysEx is skipped (with real-time inside), and cancels running status; data after is dropped
    v = parseAll({0x90, 60, 100, 0xF0, 1, 2, 3, 0xF8, 4, 0xF7, 61, 100, 0x90, 62, 100});
    REQUIRE(v.size() == 2);
    CHECK(v[1].number == 62);
    // SysEx cut off by a status byte (no F7)
    v = parseAll({0xF0, 1, 2, 3, 0xB0, 64, 127});
    REQUIRE(v.size() == 1);
    CHECK(v[0].kind == MidiMessage::ControlChange);
    // system common messages with data are skipped and cancel running status
    v = parseAll({0x90, 60, 100, 0xF2, 1, 2, 61, 100, 0xF3, 5, 0x90, 63, 100});
    REQUIRE(v.size() == 2);
    CHECK(v[1].number == 63);
    // stray data bytes without status
    CHECK(parseAll({1, 2, 3, 4}).empty());
    // pitch bend and aftertouch are parsed (the binding layer ignores them)
    v = parseAll({0xE0, 1, 2, 0xA0, 60, 9, 0xD0, 4});
    REQUIRE(v.size() == 3);
    CHECK(v[0].kind == MidiMessage::PitchBend && v[1].kind == MidiMessage::PolyPressure && v[2].kind == MidiMessage::ChannelPressure);
}

TEST_CASE("ML-T1b parser fuzz: truncation at every byte, chunk equivalence, 2000 random mutations") {
    const std::vector<uint8_t> stream = {0xF8, 0x90, 60, 100, 61, 0, 0xB2, 64, 127, 0xF0, 0x7E, 1, 2, 0xF7, 0xC1, 5,
                                         0xFE, 0xE3, 9, 9, 0xA4, 3, 3, 0xF2, 1, 1, 0xD0, 8, 0x80, 1, 1};
    const auto whole = parseAll(stream);
    CHECK(whole.size() >= 6);
    auto valid = [](const std::vector<MidiMessage>& vs) {
        for (auto& m : vs)
            if (m.channel > 15 || m.number > 127 || m.value > 127 || int(m.kind) > 6) return false;
        return true;
    };
    for (size_t n = 0; n <= stream.size(); ++n) {   // truncation at every byte
        std::vector<uint8_t> cut(stream.begin(), stream.begin() + long(n));
        auto v = parseAll(cut);
        CHECK(valid(v));
        CHECK(v.size() <= whole.size());
        for (size_t i = 0; i < v.size() && i < whole.size(); ++i)   // a prefix parses to a prefix
            if (v[i].number != whole[i].number || v[i].kind != whole[i].kind) { CHECK(false); break; }
    }
    // feeding the same bytes through one parser in arbitrary chunks gives the same messages
    MidiParser p;
    std::vector<MidiMessage> chunked;
    for (uint8_t b : stream) { MidiMessage m; if (p.feed(b, m)) chunked.push_back(m); }
    CHECK_EQ(chunked.size(), whole.size());
    std::mt19937 rng(0x5eed);
    for (int it = 0; it < 2000; ++it) {
        std::vector<uint8_t> s = stream;
        const int muts = 1 + int(rng() % 6);
        for (int k = 0; k < muts; ++k) {
            const size_t i = rng() % s.size();
            switch (rng() % 4) {
                case 0: s[i] = uint8_t(rng()); break;
                case 1: s.erase(s.begin() + long(i)); if (s.empty()) s.push_back(0x90); break;
                case 2: s.insert(s.begin() + long(i), uint8_t(rng())); break;
                default: s[i] ^= uint8_t(1u << (rng() % 8)); break;
            }
        }
        CHECK(valid(parseAll(s)));
    }
    std::vector<uint8_t> noise(65536);
    for (auto& b : noise) b = uint8_t(rng());
    CHECK(valid(parseAll(noise)));
}

TEST_CASE("ML-T2 trigger edge: CC 127 then 127 fires once; 0 then 127 fires; notes fire on press only") {
    BindingTable t;
    t.set(bind("a", -1, MidiBinding::Cc, 64));
    t.set(bind("n", 2, MidiBinding::Note, 60));
    t.set(bind("p", -1, MidiBinding::Program, 3));
    auto cc = [&](int v, int ch = 0) { return t.match(msg(MidiMessage::ControlChange, ch, 64, v)); };
    CHECK_EQ(cc(127).size(), size_t(1));
    CHECK_EQ(cc(127).size(), size_t(0));       // still held
    CHECK_EQ(cc(0).size(), size_t(0));         // release ignored
    CHECK_EQ(cc(127).size(), size_t(1));       // 0 then 127 fires
    CHECK_EQ(cc(63).size(), size_t(0));
    CHECK_EQ(cc(64).size(), size_t(1));        // threshold is >= 64
    CHECK(t.match(msg(MidiMessage::ControlChange, 0, 65, 127)).empty());   // other controller
    auto h = t.match(msg(MidiMessage::NoteOn, 2, 60, 1));
    REQUIRE(h.size() == 1);
    CHECK_NEAR(h[0].value, 1.0, 1e-12);
    CHECK(t.match(msg(MidiMessage::NoteOff, 2, 60, 0)).empty());
    CHECK(t.match(msg(MidiMessage::NoteOn, 3, 60, 100)).empty());           // wrong channel
    CHECK(t.match(msg(MidiMessage::PitchBend, 2, 60, 100)).empty());
    CHECK(t.match(msg(MidiMessage::PolyPressure, 2, 60, 100)).empty());
    CHECK_EQ(t.match(msg(MidiMessage::ProgramChange, 9, 3, 0)).size(), size_t(1));
    CHECK(t.match(msg(MidiMessage::ProgramChange, 9, 4, 0)).empty());
}

TEST_CASE("ML-T3 absolute mapping bounds") {
    BindingTable t;
    t.set(bind("v", -1, MidiBinding::Cc, 7, MidiBinding::Absolute));
    t.set(bind("w", -1, MidiBinding::Note, 36, MidiBinding::Absolute));
    for (int v = 0; v <= 127; ++v) {
        auto h = t.match(msg(MidiMessage::ControlChange, 0, 7, v));
        REQUIRE(h.size() == 1);
        CHECK(h[0].value >= 0.0 && h[0].value <= 1.0);
        CHECK_NEAR(h[0].value, v / 127.0, 1e-12);
    }
    auto lo = t.match(msg(MidiMessage::ControlChange, 0, 7, 0));
    auto hi = t.match(msg(MidiMessage::ControlChange, 0, 7, 127));
    CHECK_NEAR(lo[0].value, 0.0, 1e-12);
    CHECK_NEAR(hi[0].value, 1.0, 1e-12);
    CHECK_NEAR(t.match(msg(MidiMessage::NoteOn, 0, 36, 127))[0].value, 1.0, 1e-12);
    CHECK_NEAR(t.match(msg(MidiMessage::NoteOff, 0, 36, 0))[0].value, 0.0, 1e-12);
}

TEST_CASE("ML-T4 learn: capture, timeout, replacement, conflict and replace") {
    using M = MidiLearnModel;
    M m;
    int64_t t = 1000;
    // real-time-like and ineligible messages are not captured and do not show as lastMessage
    CHECK(m.startLearn("a", MidiBinding::Trigger, false, false, t) == M::Start::Learning);
    auto o = m.onMessage(msg(MidiMessage::PitchBend, 0, 1, 1), t + 10);
    CHECK(m.learning() != nullptr && !m.last().valid && !o.changed);
    o = m.onMessage(msg(MidiMessage::PolyPressure, 0, 1, 1), t + 11);
    CHECK(m.learning() != nullptr && !m.last().valid);
    // first eligible message is captured, not dispatched
    o = m.onMessage(msg(MidiMessage::NoteOn, 4, 60, 90), t + 20);
    CHECK(o.changed && o.hits.empty() && m.learning() == nullptr);
    REQUIRE(m.table.find("a") != nullptr);
    CHECK(m.table.find("a")->channel == -1 && m.table.find("a")->type == MidiBinding::Note && m.table.find("a")->number == 60);
    CHECK_STR(m.lastLearn(), "bound");
    CHECK(m.last().valid);
    // the bound message now fires
    CHECK_EQ(m.onMessage(msg(MidiMessage::NoteOn, 0, 60, 90), t + 30).hits.size(), size_t(1));
    // a second learn on the same action replaces its binding
    m.startLearn("a", MidiBinding::Trigger, false, true, t + 40);
    m.onMessage(msg(MidiMessage::ControlChange, 5, 64, 127), t + 50);
    CHECK_EQ(m.table.size(), size_t(1));
    CHECK(m.table.find("a")->type == MidiBinding::Cc && m.table.find("a")->channel == 5);   // exactChannel
    // timeout after 15 s
    m.startLearn("b", MidiBinding::Trigger, false, false, t + 100);
    CHECK(m.learning() != nullptr);
    m.tick(t + 100 + 14999);
    CHECK(m.learning() != nullptr);
    m.tick(t + 100 + 15000);
    CHECK(m.learning() == nullptr);
    CHECK_STR(m.lastLearn(), "timeout");
    // a message after the deadline is not captured even without a tick
    m.startLearn("b", MidiBinding::Trigger, false, false, t + 20000);
    o = m.onMessage(msg(MidiMessage::NoteOn, 0, 61, 90), t + 20000 + 15001);
    CHECK(!o.changed && m.table.find("b") == nullptr);
    // conflict: another action already owns the message
    m.startLearn("c", MidiBinding::Trigger, false, false, t + 40000);
    o = m.onMessage(msg(MidiMessage::ControlChange, 5, 64, 127), t + 40001);
    CHECK(!o.changed && m.conflict() != nullptr && m.table.find("c") == nullptr);
    CHECK_STR(m.lastLearn(), "conflict");
    REQUIRE(m.conflict()->with.size() == 1);
    CHECK_STR(m.conflict()->with[0], "a");
    // confirming (replace=true on the same action) applies the captured message
    CHECK(m.startLearn("c", MidiBinding::Trigger, true, false, t + 40002) == M::Start::Bound);
    CHECK(m.table.find("a") == nullptr && m.table.find("c") != nullptr && m.conflict() == nullptr);
    // replace=true given up front replaces without asking
    m.startLearn("d", MidiBinding::Trigger, true, false, t + 50000);
    o = m.onMessage(msg(MidiMessage::ControlChange, 8, 64, 127), t + 50001);
    CHECK(o.changed && m.table.find("c") == nullptr && m.table.find("d") != nullptr);
    // cancel
    m.startLearn("e", MidiBinding::Trigger, false, false, t + 60000);
    m.cancel();
    CHECK(m.learning() == nullptr);
    CHECK_STR(m.lastLearn(), "cancelled");
    // any vs specific channel overlap conflicts
    BindingTable bt;
    bt.set(bind("x", 3, MidiBinding::Note, 10));
    CHECK_EQ(bt.conflictsWith(bind("y", -1, MidiBinding::Note, 10)).size(), size_t(1));
    CHECK_EQ(bt.conflictsWith(bind("y", 4, MidiBinding::Note, 10)).size(), size_t(0));
    CHECK_EQ(bt.conflictsWith(bind("x", -1, MidiBinding::Note, 10)).size(), size_t(0));   // own binding is no conflict
}

TEST_CASE("ML-T5 persistence, validation and missing-action flag") {
    std::vector<std::string> warn;
    Json arr;
    Json::parse("[{\"action\":\"baroffset.cycle\",\"channel\":\"any\",\"type\":\"note\",\"number\":60,\"mode\":\"trigger\"},"
                "{\"action\":\"v.x\",\"channel\":3,\"type\":\"cc\",\"number\":7,\"mode\":\"absolute\"},"
                "{\"action\":\"nope\",\"type\":\"note\",\"number\":200},"       // bad number
                "{\"action\":\"bad id!\",\"type\":\"note\",\"number\":1},"      // bad action
                "{\"action\":\"q\",\"channel\":16,\"type\":\"note\",\"number\":1},"
                "{\"action\":\"q\",\"type\":\"sysex\",\"number\":1},"
                "{\"action\":\"dup.a\",\"type\":\"program\",\"number\":5},"
                "{\"action\":\"dup.b\",\"type\":\"program\",\"number\":5},"      // same key: last wins
                "5]", arr);
    auto t = BindingTable::fromJson(arr, [](const std::string&) { return MidiBinding::Toggle; }, warn);
    CHECK_EQ(t.size(), size_t(3));
    CHECK(t.find("dup.a") == nullptr && t.find("dup.b") != nullptr);
    CHECK(t.find("dup.b")->mode == MidiBinding::Toggle);   // default mode supplied
    CHECK(warn.size() >= 5);
    // round trip
    std::vector<std::string> w2;
    auto t2 = BindingTable::fromJson(parseJson(t.toJson().dump()), nullptr, w2);
    CHECK_EQ(t2.size(), t.size());
    CHECK(w2.empty());
    CHECK_STR(t2.toJson().dump(), t.toJson().dump());
    // limit 128
    Json big = Json::array();
    for (int i = 0; i < 200; ++i) {
        Json o = Json::object();
        o["action"] = "a" + std::to_string(i); o["type"] = "note"; o["number"] = i % 128; o["channel"] = i / 128;
        big.push(o);
    }
    std::vector<std::string> w3;
    auto tb = BindingTable::fromJson(big, nullptr, w3);
    CHECK_EQ(tb.size(), size_t(128));
    CHECK(!w3.empty());
    CHECK(!tb.set(bind("zzz", 0, MidiBinding::Cc, 1)));
    CHECK(tb.set(bind("a0", 0, MidiBinding::Cc, 1)));   // replacing an existing action works when full
    // through Core: unknown action is kept and flagged missing; learned binding persists to config
    auto dir = TEMP_DIR();
    Core core(offlineSettings(), dir.file("config.json"), dir.path());
    core.start();
    handleRequest(core, req("POST", "/api/config", patch("", "[{\"action\":\"ghost.action\",\"channel\":\"any\",\"type\":\"note\",\"number\":1,\"mode\":\"trigger\"},"
                                                         "{\"action\":\"beatonly.toggle\",\"channel\":0,\"type\":\"cc\",\"number\":9}]")));
    Json st = parseJson(handleRequest(core, req("GET", "/api/f/midilearn/state")).body);
    CHECK_JSON(st, "bindings[0].action", "ghost.action");
    CHECK_JSON(st, "bindings[0].missing", true);
    CHECK_JSON(st, "bindings[1].missing", false);
    CHECK_JSON(st, "bindings[1].mode", "toggle");      // default for a Toggle action
    CHECK_JSON(st, "bindings[1].text", "CC 9, ch 1");
    handleRequest(core, req("POST", "/api/f/midilearn/unbind", "{\"action\":\"ghost.action\"}"));
    Json cfg = parseJson(handleRequest(core, req("GET", "/api/config")).body);
    CHECK_JSON(cfg, "features.midilearn.bindings[0].action", "beatonly.toggle");
    CHECK_JSON(cfg, "features.midilearn.schema", 1);
    core.stop();
}

TEST_CASE("ML-T6a reader thread over a socketpair: bytes in, messages out, clean stop") {
    int sv[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    std::atomic<int> got{0};
    std::vector<MidiMessage> seen;
    std::mutex mu;
    int fd = sv[0];
    bool opened = false;
    auto th = std::make_unique<shunt::out::MidiInThread>(
        [&](std::string&) -> std::unique_ptr<shunt::out::IMidiInPort> {
            if (opened) return nullptr;
            opened = true;
            return std::make_unique<shunt::out::FdMidiInPort>(fd, "sock");
        },
        [&](const MidiMessage& m) { std::lock_guard<std::mutex> lk(mu); seen.push_back(m); ++got; });
    th->start();
    const uint8_t bytes[] = {0xF8, 0x90, 60, 100, 0xF8, 61, 100, 0xF0, 1, 2, 0xF7, 0xB0, 64, 127};
    CHECK(::write(sv[1], bytes, sizeof bytes) == ssize_t(sizeof bytes));
    CHECK(waitFor([&] { return got.load() >= 3; }));
    WITH_TIMEOUT(2000, { th->stop(); });
    CHECK(!th->connected());
    CHECK(th->bytesRead() == sizeof bytes);
    ::close(sv[1]);
    std::lock_guard<std::mutex> lk(mu);
    REQUIRE(seen.size() == 3);
    CHECK(seen[2].kind == MidiMessage::ControlChange);
    // stop without ever starting, and destruction of a thread that is retrying an unopenable port
    shunt::out::MidiInThread never([](std::string& e) -> std::unique_ptr<shunt::out::IMidiInPort> { e = "nope"; return nullptr; }, [](const MidiMessage&) {});
    never.stop();
    never.start();
    CHECK(waitFor([&] { return never.lastError() == "nope"; }));
    WITH_TIMEOUT(2000, { never.stop(); });
}

TEST_CASE("ML-T6 end to end: FIFO as the device, action fires, learn over the wire, stop does not hang") {
    ::signal(SIGPIPE, SIG_IGN);
    auto dir = TEMP_DIR();
    const std::string fifo = dir.file("midi.fifo");
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
    std::atomic<int> cycles{0}, toggles{0};
    std::atomic<double> level{-1};
    WITH_TIMEOUT(15000, {
        Core core(offlineSettings(), dir.file("config.json"), dir.path());
        core.start();
        auto add = [&](const std::string& id, ControlAction::Kind k, std::function<void(double)> fn) {
            ControlAction a; a.id = id; a.label = id; a.kind = k; a.group = "Test"; a.fn = std::move(fn);
            core.controls().add(a);
        };
        add("t.cycle", ControlAction::Trigger, [&](double) { ++cycles; });
        add("t.toggle", ControlAction::Toggle, [&](double) { ++toggles; });
        add("t.level", ControlAction::Value, [&](double v) { level = v; });
        handleRequest(core, req("POST", "/api/config", patch(fifo,
            "[{\"action\":\"t.cycle\",\"channel\":\"any\",\"type\":\"cc\",\"number\":64,\"mode\":\"trigger\"},"
            "{\"action\":\"t.toggle\",\"channel\":2,\"type\":\"note\",\"number\":40},"
            "{\"action\":\"t.level\",\"channel\":\"any\",\"type\":\"cc\",\"number\":7,\"mode\":\"absolute\"}]")));
        int wfd = -1;
        CHECK(waitFor([&] { wfd = ::open(fifo.c_str(), O_WRONLY | O_NONBLOCK); return wfd >= 0; }));   // reader is open
        REQUIRE(wfd >= 0);
        auto send = [&](std::vector<uint8_t> b) { CHECK(::write(wfd, b.data(), b.size()) == ssize_t(b.size())); };
        send({0xB0, 64, 127, 64, 0, 64, 127});              // press, release (running status), press
        CHECK(waitFor([&] { return cycles.load() == 2; }));
        send({0xF8, 0x92, 40, 100, 0xFE, 40, 0});           // note on, note off(vel 0)
        CHECK(waitFor([&] { return toggles.load() == 1; }));
        send({0x93, 40, 100});                              // wrong channel: ignored
        send({0xB5, 7, 127});
        CHECK(waitFor([&] { return level.load() > 0.99; }));
        CHECK_EQ(toggles.load(), 1);
        Json st = core.status().get("features").get("midilearn");
        CHECK_JSON(st, "open", true);
        CHECK_JSON(st, "lastMessage.type", "cc");
        CHECK_JSON(st, "lastMessage.number", 7);
        // learn over the wire
        Json r = parseJson(handleRequest(core, req("POST", "/api/f/midilearn/learn", "{\"action\":\"baroffset.cycle\"}")).body);
        CHECK_JSON(r, "ok", true);
        st = core.status().get("features").get("midilearn");
        CHECK_JSON_TYPE(st, "learning", Json::Object);
        send({0xB0, 20, 127});
        CHECK(waitFor([&] { return core.status().get("features").get("midilearn").get("learning").isNull(); }));
        Json state = parseJson(handleRequest(core, req("GET", "/api/f/midilearn/state")).body);
        CHECK_JSON(state, "bindings[3].action", "baroffset.cycle");
        CHECK_JSON(state, "bindings[3].text", "CC 20, ch any");
        CHECK_JSON(state, "lastLearn", "bound");
        CHECK_EQ(cycles.load(), 2);                          // the capturing message did not fire anything else
        Json cfg = parseJson(handleRequest(core, req("GET", "/api/config")).body);
        CHECK_JSON(cfg, "features.midilearn.bindings[3].number", 20);
        send({0xB0, 20, 0, 20, 127});
        CHECK(waitFor([&] { return core.status().get("features").get("midilearn").get("messages").asInt() >= 10; }));
        // disabling stops the reader; shutdown with the device still open does not hang
        handleRequest(core, req("POST", "/api/config", "{\"features\":{\"midilearn\":{\"enabled\":false}}}"));
        const int before = cycles.load();
        const uint8_t late[] = {0xB0, 64, 0, 64, 127};
        (void)!::write(wfd, late, sizeof late);          // nobody reads any more: EPIPE, ignored
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        CHECK_EQ(cycles.load(), before);
        handleRequest(core, req("POST", "/api/config", "{\"features\":{\"midilearn\":{\"enabled\":true}}}"));
        core.stop();
        ::close(wfd);
    });
}

TEST_CASE("ML-T7 API shapes, validation limits, disabled is inert") {
    auto dir = TEMP_DIR();
    Core core(offlineSettings(), dir.file("config.json"), dir.path());
    core.start();
    auto post = [&](const std::string& sub, const std::string& body) { return handleRequest(core, req("POST", "/api/f/midilearn/" + sub, body)); };
    // disabled: status is just the host keys plus inert fields, learn refused, no thread
    Json st = core.status().get("features").get("midilearn");
    CHECK_JSON(st, "enabled", false);
    CHECK_JSON(st, "open", false);
    CHECK_JSON(st, "messages", 0);
    CHECK_JSON(st, "bindings", 0);
    CHECK_EQ(post("learn", "{\"action\":\"baroffset.cycle\"}").status, 409);
    // state shape
    HttpResponse r = handleRequest(core, req("GET", "/api/f/midilearn/state"));
    CHECK_EQ(r.status, 200);
    Json s = parseJson(r.body);
    CHECK_JSON_TYPE(s, "enabled", Json::Bool);
    CHECK_JSON_TYPE(s, "ports", Json::Array);
    CHECK_JSON_TYPE(s, "port", Json::String);
    CHECK_JSON_TYPE(s, "learning", Json::Null);
    CHECK_JSON_TYPE(s, "bindings", Json::Array);
    CHECK_JSON_TYPE(s, "lastMessage", Json::Null);
    CHECK_JSON_TYPE(s, "actions", Json::Array);
    CHECK_JSON_TYPE(s, "warnings", Json::Array);
    CHECK_JSON_TYPE(s, "conflict", Json::Null);
    CHECK_JSON_TYPE(s, "actions[0].group", Json::String);
    // methods and unknown routes
    CHECK_EQ(handleRequest(core, req("POST", "/api/f/midilearn/state")).status, 405);
    CHECK_EQ(handleRequest(core, req("GET", "/api/f/midilearn/learn")).status, 405);
    CHECK_EQ(handleRequest(core, req("GET", "/api/f/midilearn/other")).status, 404);
    // enable (no port: nothing opens) and validate inputs
    handleRequest(core, req("POST", "/api/config", patch("", "[]")));
    CHECK_EQ(post("learn", "not json").status, 400);
    CHECK_EQ(post("learn", "[]").status, 400);
    CHECK_EQ(post("learn", "{}").status, 400);
    CHECK_EQ(post("learn", "{\"action\":5}").status, 400);
    CHECK_EQ(post("learn", "{\"action\":\"a b\"}").status, 400);
    CHECK_EQ(post("learn", "{\"action\":\"" + std::string(200, 'x') + "\"}").status, 400);
    CHECK_EQ(post("learn", "{\"action\":\"nope.nothing\"}").status, 404);
    CHECK_EQ(post("learn", "{\"action\":\"baroffset.cycle\",\"replace\":\"yes\"}").status, 400);
    CHECK_EQ(post("learn", std::string(5000, ' ')).status, 413);
    CHECK_EQ(post("unbind", "{}").status, 400);
    CHECK_EQ(post("port", "{\"port\":5}").status, 400);
    CHECK_EQ(post("port", "{\"port\":\"/dev/snd/../../etc/passwd\"}").status, 400);
    CHECK_EQ(post("port", "{\"port\":\"/etc/passwd\"}").status, 400);          // not a listed MIDI port
    CHECK_EQ(post("port", "{\"port\":\"\"}").status, 200);
    r = post("learn", "{\"action\":\"baroffset.cycle\"}");
    CHECK_EQ(r.status, 200);
    CHECK_JSON(r.body, "bound", false);
    s = parseJson(handleRequest(core, req("GET", "/api/f/midilearn/state")).body);
    CHECK_JSON(s, "learning.action", "baroffset.cycle");
    CHECK_JSON_TYPE(s, "learning.secondsLeft", Json::Number);
    CHECK(s.get("learning").get("secondsLeft").asInt() >= 14);
    CHECK_JSON(post("cancel", "").body, "ok", true);
    s = parseJson(handleRequest(core, req("GET", "/api/f/midilearn/state")).body);
    CHECK_JSON_TYPE(s, "learning", Json::Null);
    CHECK_JSON(s, "lastLearn", "cancelled");
    CHECK_JSON(post("unbind", "{\"action\":\"never.bound\"}").body, "removed", false);
    // 128 binding limit from settings
    std::string many = "[";
    for (int i = 0; i < 140; ++i) many += std::string(i ? "," : "") + "{\"action\":\"x" + std::to_string(i) + "\",\"type\":\"note\",\"number\":" + std::to_string(i % 128) + ",\"channel\":" + std::to_string(i / 128) + "}";
    many += "]";
    handleRequest(core, req("POST", "/api/config", patch("", many)));
    s = parseJson(handleRequest(core, req("GET", "/api/f/midilearn/state")).body);
    CHECK_EQ(int(s.get("bindings").size()), 128);
    CHECK(s.get("warnings").size() >= 1);
    CHECK_EQ(post("learn", "{\"action\":\"baroffset.cycle\"}").status, 409);   // table full
    // port that is not a char device or FIFO is refused by the reader and reported
    handleRequest(core, req("POST", "/api/config", patch(dir.file("config.json"), "[]")));
    CHECK(waitFor([&] { return core.status().get("features").get("midilearn").get("inputError").asString("").find("not a MIDI device") != std::string::npos; }));
    core.stop();
}
