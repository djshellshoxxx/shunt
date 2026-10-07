// Device-number claim state machine (ES-01 section 5). Timers come from an
// injected clock and datagrams go through an injected sender so tests run instantly.
#pragma once
#include "shunt/net/Events.h"
#include "shunt/net/DeviceTable.h"
#include <cstdint>
#include <array>
#include <string>

namespace shunt::net {

struct IClock {
    virtual ~IClock() = default;
    virtual int64_t nowNs() = 0;
};

// Sends a datagram to dstIp:dstPort (host byte order).
struct ISender {
    virtual ~ISender() = default;
    virtual bool send(uint16_t dstPort, uint32_t dstIp, const uint8_t* data, size_t len) = 0;
};

enum class Mode : uint8_t { Passive, Follow, Lead };

class ClaimMachine {
public:
    enum class State : uint8_t { Idle, Watching, Claiming, Active, Failed };
    enum class Stage : uint8_t { Hello, Stage1, Stage2, Stage3 };

    struct Config {
        Mode mode = Mode::Follow;
        std::string name = "Shunt";
        std::array<uint8_t, 6> mac{};
        uint32_t ip = 0;
        uint32_t broadcast = 0xffffffff;
        uint8_t leadNumber = 1;               // Lead mode only, 1..6
        int64_t watchNs = 4'000'000'000LL;
        int64_t stepNs = 300'000'000LL;
        int64_t keepAliveNs = 1'500'000'000LL;
    };

    ClaimMachine(const Config& cfg, ISender& sender, IClock& clock, DeviceTable& devices);

    void start();
    void stop();                         // network down -> Idle
    void tick();                         // drive timers; call often
    void onAnnounce(const Event& ev);    // KeepAlive and Claim events from 50000

    State state() const { return state_; }
    Stage stage() const { return stage_; }
    uint8_t number() const { return number_; }
    const std::string& failure() const { return failure_; }
    uint32_t sentCount() const { return sent_; }

private:
    void enterClaiming(uint8_t n);
    uint8_t firstFree(uint8_t from) const;
    void sendStep();
    void sendKeepAlive();
    void enterActive();
    void fail(const char* why);

    Config cfg_;
    ISender& sender_;
    IClock& clock_;
    DeviceTable& devices_;
    State state_ = State::Idle;
    Stage stage_ = Stage::Hello;
    uint8_t number_ = 0;
    uint8_t counter_ = 0;
    int64_t stateEnteredNs_ = 0, nextActionNs_ = 0;
    uint32_t sent_ = 0;
    std::string failure_;
    uint8_t buf_[128];
};

} // namespace shunt::net
