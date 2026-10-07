#include "shunt/net/ClaimMachine.h"
#include "shunt/net/Packets.h"

namespace shunt::net {

ClaimMachine::ClaimMachine(const Config& cfg, ISender& sender, IClock& clock, DeviceTable& devices)
    : cfg_(cfg), sender_(sender), clock_(clock), devices_(devices) {}

void ClaimMachine::start() {
    if (cfg_.mode == Mode::Passive) { state_ = State::Idle; return; }
    state_ = State::Watching;
    stateEnteredNs_ = clock_.nowNs();
    nextActionNs_ = stateEnteredNs_ + cfg_.watchNs;
    number_ = 0;
    failure_.clear();
}

void ClaimMachine::stop() {
    state_ = State::Idle;
    number_ = 0;
}

void ClaimMachine::fail(const char* why) {
    state_ = State::Failed;
    failure_ = why;
}

uint8_t ClaimMachine::firstFree(uint8_t from) const {
    for (uint8_t n = from; n <= 15; ++n)
        if (!devices_.isTaken(n)) return n;
    return 0;
}

void ClaimMachine::enterClaiming(uint8_t n) {
    if (n == 0) { fail("no free device number in 7..15"); return; }
    state_ = State::Claiming;
    number_ = n;
    stage_ = Stage::Hello;
    counter_ = 0;
    stateEnteredNs_ = clock_.nowNs();
    nextActionNs_ = stateEnteredNs_;     // first send on next tick
}

void ClaimMachine::sendStep() {
    size_t len = 0;
    ++counter_;
    switch (stage_) {
    case Stage::Hello:
        len = buildHello(buf_, cfg_.name.c_str(), true);
        break;
    case Stage::Stage1:
        len = buildClaimStage1(buf_, cfg_.name.c_str(), counter_, cfg_.mac);
        break;
    case Stage::Stage2:
        len = buildClaimStage2(buf_, cfg_.name.c_str(), cfg_.ip, cfg_.mac, number_, counter_,
                               cfg_.mode == Mode::Lead ? 0x02 : 0x01);
        break;
    case Stage::Stage3:
        len = buildClaimStage3(buf_, cfg_.name.c_str(), number_, counter_);
        break;
    }
    sender_.send(kAnnouncePort, cfg_.broadcast, buf_, len);
    ++sent_;
    if (counter_ >= 3) {
        counter_ = 0;
        switch (stage_) {
        case Stage::Hello: stage_ = Stage::Stage1; break;
        case Stage::Stage1: stage_ = Stage::Stage2; break;
        case Stage::Stage2: stage_ = Stage::Stage3; break;
        case Stage::Stage3: enterActive(); return;
        }
    }
    nextActionNs_ = clock_.nowNs() + cfg_.stepNs;
}

void ClaimMachine::enterActive() {
    state_ = State::Active;
    stateEnteredNs_ = clock_.nowNs();
    sendKeepAlive();
}

void ClaimMachine::sendKeepAlive() {
    KeepAliveParams p;
    p.name = cfg_.name.c_str();
    p.kind = DeviceKind::CDJ;
    p.number = number_;
    p.mac = cfg_.mac;
    p.ip = cfg_.ip;
    p.peerCount = uint8_t(devices_.size() + 1);
    p.cdj3000Compatible = true;
    const size_t len = buildKeepAlive(buf_, p);
    sender_.send(kAnnouncePort, cfg_.broadcast, buf_, len);
    ++sent_;
    nextActionNs_ = clock_.nowNs() + cfg_.keepAliveNs;
}

void ClaimMachine::tick() {
    const int64_t now = clock_.nowNs();
    switch (state_) {
    case State::Idle: case State::Failed:
        break;
    case State::Watching:
        if (now >= nextActionNs_) {
            if (cfg_.mode == Mode::Lead) {
                if (devices_.isTaken(cfg_.leadNumber)) fail("number in use");
                else enterClaiming(cfg_.leadNumber);
            } else {
                enterClaiming(firstFree(7));
            }
        }
        break;
    case State::Claiming:
        if (now >= nextActionNs_) sendStep();
        break;
    case State::Active:
        if (now >= nextActionNs_) sendKeepAlive();
        break;
    }
}

void ClaimMachine::onAnnounce(const Event& ev) {
    if (cfg_.mode == Mode::Passive || state_ == State::Idle || state_ == State::Failed) return;
    if (ev.type != EventType::Claim) return;
    const auto& c = ev.claim;
    const bool ours = c.mac == cfg_.mac && c.ip == cfg_.ip;
    if (state_ == State::Claiming) {
        if (c.packetType == 0x08 && c.number == number_) {
            if (cfg_.mode == Mode::Lead) fail("number in use");
            else enterClaiming(firstFree(uint8_t(number_ + 1)));
        } else if (c.packetType == 0x05) {
            enterActive();   // mixer assignment ends the claim early
        }
    } else if (state_ == State::Active) {
        if ((c.packetType == 0x02 || c.packetType == 0x04) && c.number == number_ && !ours) {
            const size_t len = buildDefend(buf_, cfg_.name.c_str(), number_);
            sender_.send(kAnnouncePort, cfg_.broadcast, buf_, len);
            ++sent_;
        }
    }
}

} // namespace shunt::net
