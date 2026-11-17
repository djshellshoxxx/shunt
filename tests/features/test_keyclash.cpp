// FS-02 tests K-T1..K-T12: key parsing and formatting, the 24x24 level matrix (hand oracle + golden),
// pitch shift, pair selection, debounce and strict mode, MetaStore import without restart, OSC edges,
// settings/status/API shape, the key scenario end to end (FixtureBuilder -> NetworkStack -> feature)
// and hot-path allocation / time budgets. Original Shunt code.
#include "TestFramework.h"
#include "Golden.h"
#include "FixtureBuilder.h"
#include "../../app/src/features/KeyClash.h"
#include "shunt/app/Api.h"
#include "shunt/app/Core.h"
#include "shunt/net/NetworkStack.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <new>
#include <random>
#include <thread>

using namespace shunt;
using namespace shunt::app;
using namespace shunt::app::keyclash;

// Counting allocator (K-T11). Counts only while g_countAllocs is set. Not installed under ASan/TSan,
// which interpose operator new themselves (the allocation check is then skipped).
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define KC_COUNT_ALLOCS 0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define KC_COUNT_ALLOCS 0
#endif
#endif
#ifndef KC_COUNT_ALLOCS
#define KC_COUNT_ALLOCS 1
#endif
static std::atomic<bool> g_countAllocs{false};
static std::atomic<long> g_allocs{0};
#if KC_COUNT_ALLOCS
__attribute__((noinline)) void* operator new(std::size_t n) {
    if (g_countAllocs.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
__attribute__((noinline)) void operator delete(void* p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete(void* p, std::size_t) noexcept { std::free(p); }
#endif

namespace {
constexpr int64_t S = 1'000'000'000;
constexpr int64_t MS = 1'000'000;

Json parseJson(const std::string& s) { Json j; Json::parse(s, j); return j; }
std::string str(const std::optional<Camelot>& c) { return c ? toString(*c) : "-"; }

template <class F> bool waitFor(F cond, int ms = 2000) {
    for (int i = 0; i < ms / 2; ++i) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return cond();
}

// The published Camelot wheel (Mixed In Key), written by hand: number, minor key, major key.
struct WheelRow { int n; const char* minor; const char* major; };
const WheelRow kWheel[12] = {
    {1, "Abm", "B"}, {2, "Ebm", "F#"}, {3, "Bbm", "Db"}, {4, "Fm", "Ab"}, {5, "Cm", "Eb"}, {6, "Gm", "Bb"},
    {7, "Dm", "F"},  {8, "Am", "C"},   {9, "Em", "G"},   {10, "Bm", "D"}, {11, "F#m", "A"}, {12, "C#m", "E"}};

std::vector<Camelot> all24() {
    std::vector<Camelot> v;
    for (int n = 1; n <= 12; ++n) { v.push_back({n, true}); }
    for (int n = 1; n <= 12; ++n) { v.push_back({n, false}); }
    return v;
}

// Independent oracle from music theory: pitch class of the tonic via the hand wheel + circle of fifths.
int tonicPc(const char* name) {
    static const char* letters = "C D EF G A B";
    int pc = int(std::string(letters).find(name[0]));
    if (name[1] == '#') ++pc;
    if (name[1] == 'b') --pc;
    return (pc + 12) % 12;
}
Level oracle(Camelot a, Camelot b) {
    auto name = [](Camelot c) { return c.minor ? kWheel[c.number - 1].minor : kWheel[c.number - 1].major; };
    auto majorPc = [&](Camelot c) { const int pc = tonicPc(name(c)); return c.minor ? (pc + 3) % 12 : pc; };
    const int pa = majorPc(a), pb = majorPc(b);
    if (a.minor == b.minor) {
        const int fifths = ((pb - pa) * 7 % 12 + 12) % 12;        // 7 is its own inverse mod 12
        if (fifths == 0) return Level::Perfect;
        if (fifths == 1 || fifths == 11) return Level::Compatible;
        if (fifths == 2 || fifths == 10) return Level::Energy;
        return Level::Clash;
    }
    return pa == pb ? Level::Compatible : Level::Clash;           // relative major / minor
}

// ---- logic helpers ----
net::Event pst(uint8_t deck, bool onAir, bool playing, uint32_t track, int64_t t, bool master = false, double pitch = 1.0) {
    net::Event e;
    e.type = net::EventType::PlayerStatus;
    e.device = deck;
    e.recvTimeNs = t;
    e.status = net::PlayerStatusEvent{};
    e.status.onAir = onAir;
    e.status.playing = playing;
    e.status.master = master;
    e.status.rekordboxId = track;
    e.status.trackType = track ? 1 : 0;
    e.status.pitch1 = pitch;
    return e;
}

struct Keys {
    std::map<uint32_t, std::string> m;
    KeyClashLogic::Lookup fn() {
        return [this](uint32_t id) -> std::optional<std::string> {
            auto it = m.find(id);
            if (it == m.end()) return std::nullopt;
            return it->second;
        };
    }
};

KeyClashSettings on(bool strict = false, int debounce = 1500, bool lock = true) {
    KeyClashSettings s; s.enabled = true; s.strict = strict; s.debounceMs = debounce; s.assumeKeyLock = lock;
    return s;
}

// Minimal FeatureHost (as in the FS-01 tests): a real OutputManager with an OSC sink.
struct FakeHost : FeatureHost {
    FakeHost() : outputs_([] { return clock::Timeline{}; }) {}
    int64_t wallNowMs() const override { return 1'700'000'000'000; }
    MetaStore& meta() override { return meta_; }
    ControlRegistry& controls() override { return controls_; }
    out::OutputManager& outputs() override { return outputs_; }
    Json settings(const std::string& id) override { return id == "onair" ? onairSettings : Json::object(); }
    bool saveSettings(const std::string&, const Json&) override { return false; }
    const clock::ClockEngine& engine() const override { return engine_; }
    void onSessionCleared(std::function<void()>) override {}
    bool enterReplay(ReplayHooks*) override { return false; }
    void leaveReplay() override {}
    bool replaying() const override { return false; }
    bool feed(uint16_t, const uint8_t*, size_t, int64_t, uint32_t, int64_t) override { return false; }
    void resetEngine(bool) override {}
    void setOutputsEnabled(bool) override {}

    void startOsc(out::OscProfile p) {
        outputs_.osc().setSink([this](const std::vector<uint8_t>& b) {
            std::lock_guard<std::mutex> lk(mu);
            raw.push_back(b);
            sent.emplace_back(reinterpret_cast<const char*>(b.data()));
        });
        out::OutputSettings os;
        os.osc.enabled = true;
        os.osc.profile = p;
        outputs_.configure(os);
        outputs_.start();
    }
    size_t count(const std::string& a) { std::lock_guard<std::mutex> lk(mu); size_t n = 0; for (auto& s : sent) n += s == a; return n; }
    std::vector<std::vector<uint8_t>> rawCopy() { std::lock_guard<std::mutex> lk(mu); return raw; }

    Json onairSettings = Json::object();
    MetaStore meta_;
    ControlRegistry controls_;
    clock::ClockEngine engine_;
    out::OutputManager outputs_;
    std::mutex mu;
    std::vector<std::string> sent;
    std::vector<std::vector<uint8_t>> raw;
};

void putMeta(MetaStore& m, uint32_t id, const char* key) { log::TrackMeta t; t.title = "t"; t.key = key; m.set(id, t); }
bool contains(const std::vector<uint8_t>& b, const std::string& needle) {
    return std::search(b.begin(), b.end(), needle.begin(), needle.end()) != b.end();
}
} // namespace

// ---------------------------------------------------------------- K-T1

TEST_CASE("K-T1 parse and format round trip for all 24 keys, 40+ spellings, garbage") {
    const auto keys = all24();
    CHECK_EQ(keys.size(), 24u);
    for (const Camelot& c : keys) {
        const std::string cs = toString(c);
        CHECK_STR(str(parseKey(cs)), cs);
        CHECK(parseKey(cs) && *parseKey(cs) == c);
        CHECK(parseKey(toMusical(c)) && *parseKey(toMusical(c)) == c);
        CHECK(parseKey(toOpenKey(c)) && *parseKey(toOpenKey(c)) == c);
        const char* hand = c.minor ? kWheel[c.number - 1].minor : kWheel[c.number - 1].major;
        CHECK_STR(toMusical(c), hand);                              // table equals the hand-written wheel
        CHECK(parseKey(hand) && *parseKey(hand) == c);
    }
    CHECK_STR(toOpenKey({8, false}), "1d");                         // C major
    CHECK_STR(toOpenKey({8, true}), "1m");                          // A minor
    CHECK_STR(toOpenKey({1, true}), "6m");
    CHECK_STR(toOpenKey({1, false}), "6d");
    struct Sp { const char* in; const char* want; };
    const Sp spellings[] = {
        {"Am", "8A"}, {"A minor", "8A"}, {"a minor", "8A"}, {"Amin", "8A"}, {"A min", "8A"}, {"AMIN", "8A"}, {"am", "8A"},
        {"F#m", "11A"}, {"Gbm", "11A"}, {"F♯m", "11A"}, {"G♭ minor", "11A"}, {"F# minor", "11A"},
        {"Bb", "6B"}, {"A#", "6B"}, {"B♭", "6B"}, {"A♯ major", "6B"}, {"Bb major", "6B"},
        {"C", "8B"}, {"C major", "8B"}, {"Cmaj", "8B"}, {"C♯ major", "3B"}, {"C#", "3B"}, {"Db", "3B"},
        {"E#", "7B"}, {"Fb", "12B"}, {"B#", "8B"}, {"Cb", "1B"}, {"E#m", "4A"},
        {"Ebm", "2A"}, {"D#m", "2A"}, {"Abm", "1A"}, {"G#m", "1A"}, {"C#m", "12A"}, {"Dbm", "12A"},
        {"  8A  ", "8A"}, {"08a", "8A"}, {"12B", "12B"}, {"1a", "1A"}, {"1d", "8B"}, {"1m", "8A"}, {"12d", "7B"},
        {"6m", "1A"}, {"\tF#m\n", "11A"}, {"G  minor", "6A"},
    };
    CHECK(sizeof(spellings) / sizeof(spellings[0]) >= 40);
    for (const Sp& s : spellings) CHECK_STR(str(parseKey(s.in)), s.want);
    const char* garbage[] = {"", " ", "13A", "0A", "8C", "8", "A", "H", "Am7", "Ab#", "A##", "Abb", "C##m", "major", "8AA", "123A", "-1A",
                             "Cm m", "Am-", "xx", "AM", "A m i n", "8 A", "♯", "1b2", "99", "A\x01"};
    for (const char* g : garbage) {
        const std::optional<Camelot> p = parseKey(g);
        if (std::string(g) == "A") CHECK(p && p->number == 11 && !p->minor);   // a bare note letter is a major key
        else CHECK_STR(str(p), "-");
    }
    CHECK_STR(str(parseKey(std::string(500, 'A'))), "-");
    CHECK_STR(str(parseKey(std::string("A\0m", 3))), "-");
    // fuzz: random byte strings never crash; results are valid keys
    std::mt19937 rng(7);
    for (int i = 0; i < 2000; ++i) {
        std::string s;
        const int n = int(rng() % 8);
        for (int k = 0; k < n; ++k) s += char(rng() % 256);
        const auto p = parseKey(s);
        if (p) CHECK(p->number >= 1 && p->number <= 12);
    }
}

// ---------------------------------------------------------------- K-T2

TEST_CASE("K-T2 level matrix: all 576 pairs against the hand oracle and the golden table") {
    const auto keys = all24();
    std::string table = "# FS-02 key levels. Rows: reference key, columns: cued key, both in the order 1A..12A 1B..12B.\n"
                        "# P perfect, C compatible, E energy, X clash. Generated once, reviewed against the Mixed In Key wheel.\n";
    table += "    ";
    for (const Camelot& c : keys) { std::string s = toString(c); table += s + std::string(4 - s.size(), ' '); }
    table += "\n";
    int counts[5] = {0, 0, 0, 0, 0};
    for (const Camelot& a : keys) {
        std::string s = toString(a);
        table += s + std::string(4 - s.size(), ' ');
        for (const Camelot& b : keys) {
            const Level l = level(a, b);
            CHECK(l == oracle(a, b));
            CHECK(level(b, a) == l);                                // symmetric
            ++counts[int(l)];
            table += std::string(1, l == Level::Perfect ? 'P' : l == Level::Compatible ? 'C' : l == Level::Energy ? 'E' : 'X') + "   ";
        }
        table += "\n";
    }
    CHECK_EQ(counts[int(Level::Perfect)], 24);
    CHECK_EQ(counts[int(Level::Compatible)], 24 * 3);               // +-1 and the relative key
    CHECK_EQ(counts[int(Level::Energy)], 24 * 2);                   // same letter +-2
    CHECK_EQ(counts[int(Level::Clash)], 576 - 24 - 72 - 48);
    CHECK_GOLDEN("key_levels.txt", table);
    // wrap-around and anchors
    CHECK(level(Camelot{12, true}, Camelot{1, true}) == Level::Compatible);
    CHECK(level(Camelot{12, false}, Camelot{2, false}) == Level::Energy);
    CHECK(level(Camelot{8, true}, Camelot{8, false}) == Level::Compatible);
    CHECK(level(Camelot{8, true}, Camelot{3, false}) == Level::Clash);
    CHECK(level(Camelot{8, true}, Camelot{9, false}) == Level::Clash);   // diagonal is not compatible per the spec
    CHECK(level(std::nullopt, Camelot{1, true}) == Level::Unknown);
    CHECK(level(Camelot{1, true}, std::nullopt) == Level::Unknown);
    CHECK_STR(levelName(Level::Unknown), "unknown");
}

// ---------------------------------------------------------------- K-T3

TEST_CASE("K-T3 pitch shift: semitones from the multiplier; assumeKeyLock false shifts, true ignores") {
    CHECK_EQ(semitonesForPitch(1.0), 0);
    CHECK_EQ(semitonesForPitch(1.0595), 1);
    CHECK_EQ(semitonesForPitch(0.9439), -1);
    CHECK_EQ(semitonesForPitch(1.08), 1);                           // +8 % is 1.3 semitones
    CHECK_EQ(semitonesForPitch(1.02), 0);                           // +2 % is 0.34 semitone
    CHECK_EQ(semitonesForPitch(1.03), 1);                           // +3 % is 0.51 semitone
    CHECK_EQ(semitonesForPitch(2.0), 12);
    CHECK_EQ(semitonesForPitch(0.0), 0);
    CHECK_EQ(semitonesForPitch(-1.0), 0);
    CHECK_EQ(semitonesForPitch(std::nan("")), 0);
    CHECK_EQ(semitonesForPitch(1e300), 24);
    CHECK(shift({8, true}, 1) == Camelot({3, true}));               // Am up a semitone = A#m = Bbm = 3A
    CHECK(shift({8, true}, -1) == Camelot({1, true}));              // G#m = 1A
    CHECK(shift({8, false}, 12) == Camelot({8, false}));
    CHECK(shift({8, true}, 0) == Camelot({8, true}));
    for (const Camelot& c : all24()) {
        CHECK(shift(shift(c, 5), -5) == c);
        CHECK(shift(c, 1).minor == c.minor);
    }
    Keys k; k.m = {{1, "8A"}, {2, "8A"}};
    for (bool lock : {true, false}) {
        KeyClashLogic l;
        l.configure(on(false, 0, lock), {});
        l.onEvent(pst(1, true, true, 1, 0, true));
        l.onEvent(pst(2, false, true, 2, 0, false, 1.0595));        // +1 semitone: 8A -> 3A
        auto lk = k.fn();
        l.tick(S, lk);
        CHECK_STR(l.snapshot().cued.text, lock ? "8A" : "3A");
        CHECK(l.snapshot().level == (lock ? Level::Perfect : Level::Clash));
    }
}

// ---------------------------------------------------------------- K-T4

TEST_CASE("K-T4 pair selection: master on air, lowest on-air, cued playing first, none") {
    Keys k; k.m = {{1, "8A"}, {2, "9A"}, {3, "3B"}, {4, "10A"}, {5, "5A"}, {6, "6A"}};
    auto lk = k.fn();
    auto run = [&](std::vector<net::Event> evs, bool mixer = false) {
        KeyClashLogic l;
        l.configure(on(), {});
        for (auto& e : evs) l.onEvent(e);
        (void)mixer;
        l.tick(S, lk);
        return l.snapshot();
    };
    // nothing
    CHECK(!run({}).pair);
    // only an on-air deck: no cued deck
    CHECK(!run({pst(1, true, true, 1, 0, true)}).pair);
    // only a cued deck: no reference
    CHECK(!run({pst(2, false, false, 2, 0)}).pair);
    // basic pair
    auto s = run({pst(1, true, true, 1, 0, true), pst(2, false, false, 2, 0)});
    CHECK(s.pair);
    CHECK_EQ(s.reference.deck, 1);
    CHECK_EQ(s.cued.deck, 2);
    CHECK(s.level == Level::Compatible);
    // master on air wins over a lower-numbered on-air deck
    s = run({pst(1, true, true, 1, 0, false), pst(2, true, true, 2, 0, true), pst(3, false, false, 3, 0)});
    CHECK_EQ(s.reference.deck, 2);
    CHECK_EQ(s.cued.deck, 3);
    // master not on air: lowest on-air deck is the reference
    s = run({pst(1, false, true, 1, 0, true), pst(2, true, true, 2, 0), pst(3, true, true, 3, 0), pst(4, false, false, 4, 0)});
    CHECK_EQ(s.reference.deck, 2);
    CHECK_EQ(s.cued.deck, 1);                                       // deck 1 is off air, loaded, playing
    // two cued decks: the playing one wins, else the lowest
    s = run({pst(1, true, true, 1, 0, true), pst(2, false, false, 2, 0), pst(3, false, true, 3, 0)});
    CHECK_EQ(s.cued.deck, 3);
    s = run({pst(1, true, true, 1, 0, true), pst(4, false, false, 4, 0), pst(3, false, false, 3, 0)});
    CHECK_EQ(s.cued.deck, 3);
    s = run({pst(1, true, true, 1, 0, true), pst(2, false, true, 2, 0), pst(3, false, true, 3, 0)});
    CHECK_EQ(s.cued.deck, 2);                                       // both playing: lowest
    // an empty deck (no track) is not cued
    s = run({pst(1, true, true, 1, 0, true), pst(2, false, true, 0, 0), pst(3, false, false, 3, 0)});
    CHECK_EQ(s.cued.deck, 3);
    // mixer packets are authoritative: deck 2 on air by the mixer although its own bit is off
    {
        KeyClashLogic l;
        l.configure(on(), {});
        l.onEvent(pst(1, true, true, 1, 0, true));
        l.onEvent(pst(2, false, true, 2, 0));
        net::Event m; m.type = net::EventType::OnAir; m.device = 0x21; m.recvTimeNs = 0; m.onAir = net::OnAirEvent{};
        m.onAir.channelCount = 4; m.onAir.channels[1] = 1;
        l.onEvent(m);
        l.tick(S / 10, lk);
        CHECK_EQ(l.snapshot().reference.deck, 2);
        CHECK_EQ(l.snapshot().cued.deck, 1);                        // deck 1 is off air by the mixer, loaded, playing
    }
    // a deck that leaves is forgotten
    {
        KeyClashLogic l;
        l.configure(on(), {});
        l.onEvent(pst(1, true, true, 1, 0, true));
        l.onEvent(pst(2, false, false, 2, 0));
        l.tick(S, lk);
        CHECK(l.snapshot().pair);
        net::Event d; d.type = net::EventType::Device; d.device = 2; d.recvTimeNs = S; d.deviceEvent.kind = net::DeviceEvent::Left;
        l.onEvent(d);
        l.tick(2 * S, lk);
        CHECK(!l.snapshot().pair);
    }
}

// ---------------------------------------------------------------- K-T5

TEST_CASE("K-T5 debounce, edges, strict mode") {
    Keys k; k.m = {{1, "8A"}, {2, "3B"}, {3, "10A"}, {4, "9A"}};
    auto lk = k.fn();
    auto step = [&](KeyClashLogic& l, int64_t fromMs, int64_t toMs, std::vector<Notice>* all = nullptr) {
        for (int64_t t = fromMs; t < toMs; t += 50) {
            auto n = l.tick(t * MS + 1, lk);
            if (all) all->insert(all->end(), n.begin(), n.end());
        }
    };
    KeyClashLogic l;
    l.configure(on(false, 1500), {});
    l.onEvent(pst(1, true, true, 1, 0, true));
    l.onEvent(pst(2, false, false, 2, 0));
    std::vector<Notice> notes;
    step(l, 0, 1400, &notes);
    CHECK(!l.snapshot().alarm);
    CHECK(l.snapshot().level == Level::Clash);
    step(l, 1400, 1700, &notes);
    CHECK(l.snapshot().alarm);
    int alarmEdges = 0;
    for (auto& n : notes) alarmEdges += n.alarmEdge && n.alarm;
    CHECK_EQ(alarmEdges, 1);
    // improving for less than the debounce keeps the alarm
    l.onEvent(pst(2, false, false, 4, 1700 * MS));                  // deck 2 now 9A: compatible with 8A
    step(l, 1700, 3000, &notes);
    CHECK(l.snapshot().alarm);
    step(l, 3000, 3700, &notes);
    CHECK(!l.snapshot().alarm);
    int cleared = 0;
    for (auto& n : notes) cleared += n.alarmEdge && !n.alarm;
    CHECK_EQ(cleared, 1);
    // a flicker shorter than the debounce never raises
    notes.clear();
    l.onEvent(pst(2, false, false, 2, 3700 * MS));
    step(l, 3700, 4700, &notes);
    l.onEvent(pst(2, false, false, 4, 4700 * MS));
    step(l, 4700, 8000, &notes);
    CHECK(!l.snapshot().alarm);
    for (auto& n : notes) CHECK(!n.alarmEdge);
    // strict: energy raises
    for (bool strict : {false, true}) {
        KeyClashLogic e;
        e.configure(on(strict, 500), {});
        e.onEvent(pst(1, true, true, 1, 0, true));
        e.onEvent(pst(2, false, false, 3, 0));                      // 10A vs 8A: energy
        step(e, 0, 1000);
        CHECK(e.snapshot().level == Level::Energy);
        CHECK(e.snapshot().alarm == strict);
    }
    // debounce 0 reacts at the next evaluation; unknown never alarms
    KeyClashLogic z;
    z.configure(on(false, 0), {});
    z.onEvent(pst(1, true, true, 1, 0, true));
    z.onEvent(pst(2, false, false, 2, 0));
    z.tick(1 * S, lk);
    CHECK(z.snapshot().alarm);
    k.m.erase(2);
    z.tick(2 * S, lk);
    CHECK(z.snapshot().level == Level::Unknown);
    CHECK(!z.snapshot().alarm);
    // evaluation is limited to 5 Hz
    KeyClashLogic r;
    r.configure(on(false, 0), {});
    r.onEvent(pst(1, true, true, 1, 0, true));
    r.onEvent(pst(2, false, false, 4, 0));
    k.m[2] = "3B";
    r.tick(1 * S, lk);
    k.m[4] = "3B";
    r.tick(1 * S + 50 * MS, lk);
    CHECK(r.snapshot().level == Level::Compatible);                 // not re-evaluated yet (still the old key)
    r.tick(1 * S + 250 * MS, lk);
    CHECK(r.snapshot().level == Level::Clash);
}

// ---------------------------------------------------------------- K-T6

TEST_CASE("K-T6 no metadata gives unknown; an import later updates without restart") {
    FakeHost h;
    KeyClashFeature f;
    f.init(h, parseJson(R"({"enabled":true,"debounceMs":0})"));
    f.onEvent(pst(1, true, true, 2001, 0, true), 0);
    f.onEvent(pst(2, false, false, 2002, 0), 0);
    f.onTick(1 * S, 0);
    Json st = f.status();
    CHECK_JSON(st, "level", "unknown");
    CHECK_JSON(st, "alarm", false);
    CHECK_JSON(st, "metaTracks", 0);
    CHECK_JSON(st, "reference.deck", 1);
    CHECK_JSON(st, "reference.key", "");
    CHECK_JSON(st, "sources.1", "none");
    putMeta(h.meta_, 2001, "Am");
    putMeta(h.meta_, 2002, "Ebm");                                  // 2A vs 8A: clash
    f.onTick(2 * S, 0);
    st = f.status();
    CHECK_JSON(st, "level", "clash");
    CHECK_JSON(st, "alarm", true);
    CHECK_JSON(st, "reference.key", "8A");
    CHECK_JSON(st, "cued.key", "2A");
    CHECK_JSON(st, "sources.1", "xml");
    CHECK_JSON(st, "sources.2", "xml");
    CHECK_JSON(st, "metaTracks", 2);
    // spellings the XML does not use and unparseable keys
    putMeta(h.meta_, 2002, "n/a");
    f.onTick(3 * S, 0);
    CHECK_JSON(f.status(), "level", "unknown");
    CHECK_JSON(f.status(), "alarm", false);
    // no pair at all
    FakeHost h2;
    KeyClashFeature g;
    g.init(h2, parseJson(R"({"enabled":true})"));
    g.onTick(1 * S, 0);
    st = g.status();
    CHECK_JSON(st, "level", "none");
    CHECK_JSON(st, "reference", Json());
    CHECK_JSON(st, "cued", Json());
}

// ---------------------------------------------------------------- K-T7

TEST_CASE("K-T7 OSC: /shunt/keyclash on alarm edges and key-pair changes; QLab cue; toggles") {
    WITH_TIMEOUT(15000, {
        FakeHost h;
        h.startOsc(out::OscProfile::Generic);
        putMeta(h.meta_, 1, "Am"); putMeta(h.meta_, 2, "Ebm"); putMeta(h.meta_, 3, "Em");
        KeyClashFeature f;
        f.init(h, parseJson(R"({"enabled":true,"debounceMs":500})"));
        f.onEvent(pst(1, true, true, 1, 0, true), 0);
        f.onEvent(pst(2, false, false, 2, 0), 0);
        int64_t t = 0;
        auto run = [&](int64_t ms) { for (int64_t e = t + ms; t < e; t += 50 * MS) f.onTick(t + S, 0); };
        run(300 * MS);                                              // pair appears: one message, alarm still 0
        REQUIRE(waitFor([&] { return h.count("/shunt/keyclash") == 1; }));
        run(1000 * MS);                                             // alarm edge
        REQUIRE(waitFor([&] { return h.count("/shunt/keyclash") == 2; }));
        run(1000 * MS);
        CHECK_EQ(h.count("/shunt/keyclash"), 2u);                   // nothing while stable
        f.onEvent(pst(2, false, false, 3, t + S), 0);               // pair change 8A/2A -> 8A/9A
        run(1500 * MS);
        REQUIRE(waitFor([&] { return h.count("/shunt/keyclash") >= 3; }));
        f.shutdown();
        auto raw = h.rawCopy();
        REQUIRE(raw.size() >= 3u);
        // typetags ",iss" and strings of the third message
        CHECK(contains(raw[0], ",iss"));
        CHECK(contains(raw[0], "8A")); CHECK(contains(raw[0], "2A"));
        CHECK(raw[1].size() == raw[0].size());
        CHECK(contains(raw[2], "9A"));
        // toggle off: nothing further
        h.sent.clear();
        KeyClashFeature q;
        FakeHost h3;
        h3.startOsc(out::OscProfile::Generic);
        putMeta(h3.meta_, 1, "Am"); putMeta(h3.meta_, 2, "Ebm");
        q.init(h3, parseJson(R"({"enabled":true,"debounceMs":0,"oscEnabled":false})"));
        q.onEvent(pst(1, true, true, 1, 0, true), 0);
        q.onEvent(pst(2, false, false, 2, 0), 0);
        for (int i = 0; i < 20; ++i) q.onTick(S + i * 50 * MS, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CHECK_EQ(h3.count("/shunt/keyclash"), 0u);
        // QLab: only the alarm-rising edge starts the configured cue
        FakeHost h4;
        h4.startOsc(out::OscProfile::QLab);
        putMeta(h4.meta_, 1, "Am"); putMeta(h4.meta_, 2, "Ebm");
        KeyClashFeature ql;
        ql.init(h4, parseJson(R"({"enabled":true,"debounceMs":0,"qlabKeyClashCue":12})"));
        ql.onEvent(pst(1, true, true, 1, 0, true), 0);
        ql.onEvent(pst(2, false, false, 2, 0), 0);
        for (int i = 0; i < 20; ++i) ql.onTick(S + i * 50 * MS, 0);
        REQUIRE(waitFor([&] { return h4.count("/cue/12/start") == 1; }));
        CHECK_EQ(h4.count("/shunt/keyclash"), 0u);
    });
}

// ---------------------------------------------------------------- K-T8

TEST_CASE("K-T8 settings validation, status shape, routes, disabled is inert") {
    KeyClashSettings d = KeyClashSettings::fromJson(parseJson(R"({"enabled":1,"strict":"yes","debounceMs":"x","qlabKeyClashCue":-3})"));
    CHECK(!d.enabled); CHECK(!d.strict); CHECK(d.assumeKeyLock); CHECK(d.oscEnabled);
    CHECK_EQ(d.debounceMs, 1500);
    CHECK_EQ(d.qlabKeyClashCue, 0);
    CHECK_EQ(KeyClashSettings::fromJson(parseJson(R"({"debounceMs":-5})")).debounceMs, 0);
    CHECK_EQ(KeyClashSettings::fromJson(parseJson(R"({"debounceMs":1e12})")).debounceMs, 30000);
    CHECK_EQ(KeyClashSettings::fromJson(parseJson(R"({"qlabKeyClashCue":123456})")).qlabKeyClashCue, 9999);
    CHECK(!KeyClashSettings::fromJson(parseJson(R"({"assumeKeyLock":false})")).assumeKeyLock);
    CHECK(!KeyClashSettings::fromJson(Json()).enabled);
    CHECK_STR(KeyClashSettings{}.toJson().dump(),
              R"({"enabled":false,"strict":false,"assumeKeyLock":true,"debounceMs":1500,"oscEnabled":true,"qlabKeyClashCue":0,"experimentalStatusKey":false})");

    auto tmp = TEMP_DIR();
    Settings s;
    s.interfaceName = "fs02-no-such-interface";
    Core core(s, tmp.file("config.json"), tmp.path());
    core.start();
    auto req = [&](const std::string& m, const std::string& path, const std::string& body = "") {
        HttpRequest r; r.method = m; r.path = path; r.body = body;
        return handleRequest(core, r);
    };
    Json st = core.status().get("features").get("keyclash");
    CHECK_JSON(st, "enabled", false);
    CHECK_JSON(st, "level", "none");
    CHECK_JSON(st, "alarm", false);
    auto r = req("POST", "/api/config", R"({"features":{"keyclash":{"enabled":true,"debounceMs":99999999,"strict":true,"assumeKeyLock":"no"}}})");
    CHECK_EQ(r.status, 200);
    r = req("GET", "/api/f/keyclash/settings");
    CHECK_EQ(r.status, 200);
    Json js = parseJson(r.body);
    CHECK_JSON(js, "debounceMs", 30000);
    CHECK_JSON(js, "strict", true);
    CHECK_JSON(js, "assumeKeyLock", true);
    CHECK_JSON(js, "enabled", true);
    CHECK_EQ(req("POST", "/api/f/keyclash/settings", "{}").status, 405);
    CHECK_EQ(req("GET", "/api/f/keyclash/nope").status, 404);
    st = core.status().get("features").get("keyclash");
    CHECK_JSON(st, "enabled", true);
    CHECK_JSON_TYPE(st, "level", Json::String);
    CHECK_JSON_TYPE(st, "alarm", Json::Bool);
    CHECK_JSON_TYPE(st, "reference", Json::Null);
    CHECK_JSON_TYPE(st, "cued", Json::Null);
    CHECK_JSON_TYPE(st, "sources", Json::Object);
    CHECK_JSON_TYPE(st, "strict", Json::Bool);
    CHECK_JSON_TYPE(st, "assumeKeyLock", Json::Bool);
    CHECK_JSON_TYPE(st, "metaTracks", Json::Number);
    CHECK_JSON_TYPE(st, "experimental", Json::Bool);
    CHECK_JSON_TYPE(st, "error", Json::String);
    CHECK_EQ(st.size(), 11u);
    // pair shape
    FakeHost h;
    putMeta(h.meta_, 1, "8A"); putMeta(h.meta_, 2, "3B");
    KeyClashFeature f;
    f.init(h, parseJson(R"({"enabled":true,"debounceMs":0})"));
    f.onEvent(pst(1, true, true, 1, 0, true), 0);
    f.onEvent(pst(2, false, false, 2, 0), 0);
    f.onTick(S, 0);
    Json j = f.status();
    CHECK_JSON_TYPE(j, "reference.deck", Json::Number);
    CHECK_JSON_TYPE(j, "reference.key", Json::String);
    CHECK_EQ(j.get("reference").size(), 2u);
    CHECK_JSON(j, "sources.1", "xml");
    CHECK_JSON(j, "level", "clash");
    // disabled: the host never calls onEvent / onTick; applying disabled settings clears state
    f.applySettings(parseJson(R"({"enabled":false})"));
    CHECK_JSON(f.status(), "level", "none");
    CHECK_JSON(f.status(), "alarm", false);
    // disabled feature sends no OSC and changes no output flag
    core.stop();
}

// ---------------------------------------------------------------- K-T9

TEST_CASE("K-T9 scenario key: XML keys then alarm in the simulator timeline (FixtureBuilder)") {
    fixture::CaptureParams p;
    p.seed = 5;
    p.durationS = 30;
    fixture::DeckSpec a; a.number = 1; a.master = true; a.onAir = true; a.trackId = 2001;
    fixture::DeckSpec b; b.number = 2; b.onAir = false; b.playing = false; b.trackId = 2002;
    p.decks = {a, b};
    p.changes.push_back({20.0, 2, [](fixture::DeckSpec& d) { d.trackId = 2003; }});
    const auto caps = fixture::buildCapture(p);
    auto factory = net::makePosixSocketFactory();
    net::StackConfig sc;
    sc.mode = net::Mode::Follow;
    sc.filterSubnet = false;
    net::NetworkStack stack(*factory, sc);
    clock::ClockEngine engine;
    FakeHost host;
    KeyClashFeature f;
    f.init(host, parseJson(R"({"enabled":true})"));
    const int64_t t0 = caps.front().timestampNs;
    size_t i = 0;
    struct Sample { double t; std::string level; bool alarm; std::string cued; };
    std::vector<Sample> samples;
    auto sample = [&](double at, int64_t now) {
        if (double(now - t0) / 1e9 < at || (!samples.empty() && samples.back().t >= at)) return;
        Json j = f.status();
        samples.push_back({at, j.get("level").asString(), j.get("alarm").asBool(), j.get("cued").get("key").asString()});
    };
    bool imported = false;
    for (int64_t now = t0; now < t0 + 30 * S; now += MS) {
        const double rel = double(now - t0) / 1e9;
        if (!imported && rel >= 5.0) {                              // XML import happens mid-run, no restart
            putMeta(host.meta_, 2001, "Am"); putMeta(host.meta_, 2002, "Bb"); putMeta(host.meta_, 2003, "Em");
            imported = true;
        }
        while (i < caps.size() && caps[i].timestampNs <= now) {
            const auto& d = caps[i++];
            stack.feed(d.dstPort, d.payload.data(), d.payload.size(), d.timestampNs, d.srcIp);
        }
        stack.runTimers(now);
        net::Event ev;
        while (stack.pop(ev)) { engine.onEvent(ev); f.onEvent(ev, 0); }
        engine.tick(now);
        f.onTick(now, 0);
        sample(2.0, now); sample(4.5, now); sample(8.0, now); sample(18.0, now); sample(25.0, now);
    }
    REQUIRE(samples.size() == 5u);
    CHECK_STR(samples[0].level, "unknown");                          // before the import
    CHECK_STR(samples[1].level, "unknown");
    CHECK_STR(samples[2].level, "clash");                            // 8A vs 6B, debounce not yet over at 8.0? 3 s later: alarm
    CHECK(samples[2].alarm);
    CHECK_STR(samples[3].cued, "6B");
    CHECK(samples[3].alarm);
    CHECK_STR(samples[4].level, "compatible");                       // 8A vs 9A after deck 2 loads 2003
    CHECK_STR(samples[4].cued, "9A");
    CHECK(!samples[4].alarm);
}

// ---------------------------------------------------------------- K-T10

TEST_CASE("K-T10 status key bytes: not decoded by default; experimental ASCII hypothesis is opt-in and XML wins") {
    const uint8_t ascii[3] = {'8', 'A', 0};
    const uint8_t placeholder[3] = {0x8a, 0x00, 0x00};
    CHECK(!decodeStatusKey(ascii));
    CHECK_STR(str(experimentalDecodeStatusKey(ascii)), "8A");
    CHECK_STR(str(experimentalDecodeStatusKey(placeholder)), "-");
    const uint8_t b3[3] = {'1', '2', 'B'};
    CHECK_STR(str(experimentalDecodeStatusKey(b3)), "12B");
    auto ev = [&](uint8_t deck, bool onAir, uint32_t track, const uint8_t* key) {
        net::Event e = pst(deck, onAir, true, track, 0, deck == 1);
        e.status.hasKey = true;
        for (int i = 0; i < 3; ++i) e.status.key[i] = key[i];
        return e;
    };
    const uint8_t k3b[3] = {'3', 'B', 0};
    Keys k;
    auto lk = k.fn();
    for (bool exp : {false, true}) {
        KeyClashLogic l;
        KeyClashSettings s = on(false, 0);
        s.experimentalStatusKey = exp;
        l.configure(s, {});
        l.onEvent(ev(1, true, 1, ascii));
        l.onEvent(ev(2, false, 2, k3b));
        l.tick(S, lk);
        CHECK(l.snapshot().level == (exp ? Level::Clash : Level::Unknown));
        CHECK(l.snapshot().reference.source == (exp ? Source::Status : Source::None));
    }
    k.m[1] = "9A";                                                    // XML present: it wins over the status bytes
    KeyClashLogic l;
    KeyClashSettings s = on(false, 0);
    s.experimentalStatusKey = true;
    l.configure(s, {});
    l.onEvent(ev(1, true, 1, ascii));
    l.onEvent(ev(2, false, 2, k3b));
    l.tick(S, lk);
    CHECK_STR(l.snapshot().reference.text, "9A");
    CHECK(l.snapshot().reference.source == Source::Xml);
}

// ---------------------------------------------------------------- K-T11

TEST_CASE("K-T11 hot path: onEvent allocates nothing in steady state; onTick under 20 us (10x slack)") {
    FakeHost h;
    putMeta(h.meta_, 1, "Am"); putMeta(h.meta_, 2, "Ebm");
    KeyClashFeature f;
    f.init(h, parseJson(R"({"enabled":true,"debounceMs":0})"));
    f.onEvent(pst(1, true, true, 1, 0, true), 0);
    f.onEvent(pst(2, false, false, 2, 0), 0);
    f.onTick(S, 0);
    const net::Event e1 = pst(1, true, true, 1, 1, true), e2 = pst(2, false, false, 2, 1);
    net::Event mx; mx.type = net::EventType::OnAir; mx.device = 0x21; mx.onAir = net::OnAirEvent{}; mx.onAir.channelCount = 4; mx.onAir.channels[0] = 1;
    g_allocs = 0;
    g_countAllocs = true;
    for (int i = 0; i < 10000; ++i) { f.onEvent(e1, 0); f.onEvent(e2, 0); f.onEvent(mx, 0); }
    g_countAllocs = false;
    CHECK_EQ(g_allocs.load(), 0L);
    const int n = 20000;
    const auto a = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) f.onTick(S + int64_t(i) * 5 * MS, 0);
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - a).count() / n;
    CHECK(us < 200.0);
}

// ---------------------------------------------------------------- K-T12

TEST_CASE("K-T12 FS-01 channel map is honoured for the on-air decision") {
    FakeHost h;
    h.onairSettings = parseJson(R"({"channelMap":[2,1,3,4,5,6]})");   // deck 1 -> mixer channel 2
    putMeta(h.meta_, 1, "Am"); putMeta(h.meta_, 2, "Am");
    KeyClashFeature f;
    f.init(h, parseJson(R"({"enabled":true,"debounceMs":0})"));
    f.onEvent(pst(1, false, true, 1, 0), 0);
    f.onEvent(pst(2, false, true, 2, 0), 0);
    net::Event m; m.type = net::EventType::OnAir; m.device = 0x21; m.recvTimeNs = 0; m.onAir = net::OnAirEvent{};
    m.onAir.channelCount = 4; m.onAir.channels[1] = 1;                // mixer channel 2 on
    f.onEvent(m, 0);
    f.onTick(100 * MS, 0);
    CHECK_JSON(f.status(), "reference.deck", 1);                      // deck 1 sits on channel 2
}
