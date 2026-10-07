#include "shunt/net/MasterTracker.h"
#include "shunt/net/Packets.h"
#include <cmath>

namespace shunt::net {

std::optional<Event> MasterTracker::change(uint8_t to, int64_t nowNs, bool inferred) {
    if (to == master_) return std::nullopt;
    Event e;
    e.type = EventType::MasterChanged;
    e.recvTimeNs = nowNs;
    e.device = to;
    e.masterChanged.from = master_;
    e.masterChanged.to = to;
    e.masterChanged.inferred = inferred;
    master_ = to;
    handoffTo_ = 0xff;
    if (to == 0) state_ = State::NoMaster;
    else if (to == kMixerDeviceNumber) state_ = State::MixerMaster;
    else state_ = State::Master;
    lastMasterStatusNs_ = nowNs;
    return e;
}

std::optional<Event> MasterTracker::onStatus(uint8_t n, bool masterFlag, uint8_t handoffTo, int64_t nowNs) {
    switch (state_) {
    case State::NoMaster:
    case State::MixerMaster:
        if (masterFlag) return change(n, nowNs, false);
        break;
    case State::Master:
        if (n == master_) {
            lastMasterStatusNs_ = nowNs;
            if (handoffTo != 0xff && handoffTo != 0 && handoffTo != n) {
                state_ = State::Handoff;
                handoffTo_ = handoffTo;
            }
        } else if (masterFlag) {
            return change(n, nowNs, false);   // direct takeover without a handoff
        }
        break;
    case State::Handoff:
        if (n == master_) lastMasterStatusNs_ = nowNs;
        if (masterFlag && n == handoffTo_) return change(n, nowNs, false);
        if (masterFlag && n != master_) return change(n, nowNs, false);
        break;
    }
    return std::nullopt;
}

std::optional<Event> MasterTracker::onEvent(Event& ev) {
    switch (ev.type) {
    case EventType::PlayerStatus:
        if (passive_) return std::nullopt;
        return onStatus(ev.device, ev.status.master || ev.status.masterMeaning != 0,
                        ev.status.handoffTo, ev.recvTimeNs);
    case EventType::MixerStatus:
        if (passive_) return std::nullopt;
        mixerPresent_ = true;
        return onStatus(ev.device, ev.mixer.master, ev.mixer.handoffTo, ev.recvTimeNs);
    case EventType::Beat: {
        ev.beat.isMaster = (master_ != 0 && ev.device == master_);
        if (!passive_) return std::nullopt;
        // Passive inference: the player whose beat tempo equals the mixer's beat tempo,
        // else the most recently playing player.
        if (ev.device == kMixerDeviceNumber) {
            mixerBeatBpm_ = ev.beat.effectiveBpm;
            return std::nullopt;
        }
        lastPlayingPlayer_ = ev.device;
        uint8_t candidate = lastPlayingPlayer_;
        if (mixerBeatBpm_ > 0 && std::fabs(ev.beat.effectiveBpm - mixerBeatBpm_) < 0.011)
            candidate = ev.device;
        else if (master_ != 0 && mixerBeatBpm_ > 0)
            return std::nullopt;   // keep the current inferred master while a mixer tempo exists
        auto out = change(candidate, ev.recvTimeNs, true);
        ev.beat.isMaster = (ev.device == master_);
        return out;
    }
    default:
        return std::nullopt;
    }
}

std::optional<Event> MasterTracker::tick(int64_t nowNs) {
    if ((state_ == State::Master || state_ == State::Handoff) && !passive_ &&
        nowNs - lastMasterStatusNs_ > kStatusTimeoutNs) {
        return change(mixerPresent_ ? kMixerDeviceNumber : 0, nowNs, false);
    }
    return std::nullopt;
}

} // namespace shunt::net
