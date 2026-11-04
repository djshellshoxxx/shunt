// ShuntSim engine and scenario registry (FS-TEST section 2, RS-05).
// A scenario is a setup function that configures decks/mixer state and schedules
// timed actions with Sim::at(). The engine runs on a virtual clock: every event
// happens at its exact scheduled time, so output (and --record files) depend only
// on the options and --seed, never on wall-clock scheduling. --time-scale maps
// virtual to wall time (1 = real time, 0 = as fast as possible).
// Original Shunt code.
#pragma once
#include "shunt/net/Packets.h"
#include "shunt/net/Pcapng.h"
#include "shunt/net/Socket.h"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace shuntsim {

constexpr int64_t MS = 1'000'000;

struct Deck {
    uint8_t number;
    const char* name;
    std::array<uint8_t, 6> mac;
    uint32_t ip;
    double baseBpm;
    double pitch = 1.0;
    bool playing = true;
    bool master = false;
    uint8_t handoffTo = 0xff;
    int64_t nextBeatNs = 0;
    int beatInBar = 1;           // true position in the bar (1..4)
    int barShift = 0;            // reported beatInBar = true shifted by this many beats (wrongbar)
    bool derivedBar = false;     // recompute beatInBar from beatNumber after every beat
    uint32_t beatNumber = 1;
    bool loopOn = false;         // legacy loop (wraps on beats 9..12)
    uint32_t loopStart = 0;      // generic loop: beatNumber wraps within [loopStart, loopStart+loopLen)
    uint32_t loopLen = 0;
    uint32_t trackId = 0;        // 0 = 1000 + number
    bool hasKey = false;
    uint8_t key[3] = {0, 0, 0};
    int onAirFlag = -1;          // -1 follows `playing`, else the F-byte on-air bit
    double bpm() const { return baseBpm * pitch; }
    double periodNs() const { return 60e9 / bpm(); }
    int reportedBeatInBar() const { return (beatInBar - 1 + barShift % 4 + 4) % 4 + 1; }
};

struct Options {
    std::string dest;                       // empty: do not send (record only)
    std::string scenario = "handoff";
    double durationS = -1;                  // <0: scenario default
    double jitterMs = 0;
    double lossPct = 0;
    double outlierPct = 0;
    double outlierMs = 100;
    unsigned seed = 1;
    bool quiet = false;
    bool verbose = false;
    std::string record;
    double timeScale = 1.0;
    bool keyBytesSet = false;
    uint8_t keyBytes[3] = {0, 0, 0};
    bool barShiftSet = false;
    int barShift = 0;
};

class Sim {
public:
    explicit Sim(const Options& o);
    bool init(std::string* err);     // sockets, recorder, decks, scenario setup
    void run();
    void summary() const;

    // ---- scenario API
    Options opt;
    Deck decks[4];
    bool mixerOnAirPackets = true;   // send 0x03 mixer on-air packets
    int mixerChannel[4] = {-1, -1, -1, -1};   // -1 follows deck playing, else 0/1
    int nudgeBeats = 0, rampBeats = 0;
    double durationS = 20;           // scenario default, overridden by --duration
    const char* scenarioName = "";

    void at(double tSeconds, std::function<void(Sim&)> fn);
    double t() const { return double(now_) / 1e9; }
    int64_t nowNs() const { return now_; }
    void log(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    Deck& deck(int number) { return decks[number - 1]; }
    // Channel on/off keeps mixer packet state and the deck F-byte on-air bit in step.
    void setChannel(int deckNumber, bool on);
    void loadTrack(int deckNumber, uint32_t id, const uint8_t key[3] = nullptr);
    void setMaster(int deckNumber);          // clears the others

private:
    struct Action { double t; std::function<void(Sim&)> fn; };
    struct Pending { int64_t at; uint64_t seq; uint32_t srcIp; uint16_t port; std::vector<uint8_t> data; };
    void emit(uint32_t srcIp, uint16_t port, const uint8_t* data, size_t len, int64_t delayNs = 0);
    void sendNow(uint32_t srcIp, uint16_t port, const uint8_t* data, size_t len);
    void waitUntil(int64_t virtualNs);
    const Deck* masterDeck() const;

    std::unique_ptr<shunt::net::ISocketFactory> factory_;
    std::unique_ptr<shunt::net::IUdpSocket> sock_;
    shunt::net::PcapngWriter writer_;
    uint32_t destIp_ = 0;
    std::mt19937 rng_;
    std::vector<Action> actions_;
    size_t nextAction_ = 0;
    std::vector<Pending> pending_;
    uint64_t seq_ = 0;
    int64_t now_ = 0;
    int64_t wallStart_ = 0;
    int beatsSent_ = 0, beatsDropped_ = 0;
};

using ScenarioSetup = void (*)(Sim&);
struct ScenarioInfo { const char* name; ScenarioSetup setup; };
std::vector<ScenarioInfo>& scenarioRegistry();
struct ScenarioRegistrar {
    ScenarioRegistrar(const char* name, ScenarioSetup fn) { scenarioRegistry().push_back({name, fn}); }
};

} // namespace shuntsim

#define SHUNTSIM_CAT2(a, b) a##b
#define SHUNTSIM_CAT(a, b) SHUNTSIM_CAT2(a, b)
// REGISTER_SCENARIO("name", setupFunction) at namespace scope in tools/shuntsim/scenarios/*.cpp.
#define REGISTER_SCENARIO(name, fn) \
    static ::shuntsim::ScenarioRegistrar SHUNTSIM_CAT(shuntsim_reg_, __LINE__)(name, fn)
