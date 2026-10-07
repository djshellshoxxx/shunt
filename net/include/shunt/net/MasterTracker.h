// Tempo-master tracking (ES-01 section 6).
#pragma once
#include "shunt/net/Events.h"
#include <optional>

namespace shunt::net {

class MasterTracker {
public:
    enum class State : uint8_t { NoMaster, Master, Handoff, MixerMaster };
    static constexpr int64_t kStatusTimeoutNs = 2'000'000'000LL;

    // Feed PlayerStatus / MixerStatus / Beat events. Returns a MasterChanged
    // event on transitions. Beat events are tagged isMaster in place.
    std::optional<Event> onEvent(Event& ev);
    // Time-driven expiry; returns a MasterChanged event if the master went silent.
    std::optional<Event> tick(int64_t nowNs);

    void setMixerPresent(bool present) { mixerPresent_ = present; }
    void setPassive(bool passive) { passive_ = passive; }

    State state() const { return state_; }
    uint8_t master() const { return master_; }
    uint8_t handoffTarget() const { return handoffTo_; }
    bool inferred() const { return passive_; }

private:
    std::optional<Event> change(uint8_t to, int64_t nowNs, bool inferred);
    std::optional<Event> onStatus(uint8_t n, bool masterFlag, uint8_t handoffTo, int64_t nowNs);

    State state_ = State::NoMaster;
    uint8_t master_ = 0, handoffTo_ = 0xff;
    int64_t lastMasterStatusNs_ = 0;
    bool mixerPresent_ = false;
    bool passive_ = false;
    // Passive inference
    double mixerBeatBpm_ = 0;
    uint8_t lastPlayingPlayer_ = 0;
};

} // namespace shunt::net
