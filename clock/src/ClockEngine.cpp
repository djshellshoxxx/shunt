#include "shunt/clock/ClockEngine.h"
#include "shunt/net/Packets.h"
#include <cmath>
#include <algorithm>

namespace shunt::clock {

namespace {
constexpr double kMsNs = 1e6;
constexpr double kMinGateNs = 25.0 * kMsNs;
constexpr double kNudgeNs = 10.0 * kMsNs;
constexpr double kDefaultPeriodNs = 60e9 / 120.0;
constexpr int64_t kCoastToPauseNs = 1'000'000'000LL;
constexpr double kConfidenceTauNs = 2e9;
}

// ---------------------------------------------------------------- PlayerTrack

double PlayerTrack::mad() const {
    if (residualCount < 4) return 0;
    std::array<double, 16> v{};
    for (int i = 0; i < residualCount; ++i) v[i] = residualHistory[i];
    std::sort(v.begin(), v.begin() + residualCount);
    const double med = v[residualCount / 2];
    for (int i = 0; i < residualCount; ++i) v[i] = std::fabs(v[i] - med);
    std::sort(v.begin(), v.begin() + residualCount);
    return v[residualCount / 2] * 1.4826;
}

double PlayerTrack::rmsResidual() const {
    if (residualCount == 0) return 0;
    double s = 0;
    for (int i = 0; i < residualCount; ++i) s += residualHistory[i] * residualHistory[i];
    return std::sqrt(s / residualCount);
}

double PlayerTrack::hitRate() const {
    if (hitCount == 0) return 1.0;
    int h = 0;
    for (int i = 0; i < hitCount; ++i) h += hits[i];
    return double(h) / hitCount;
}

// ---------------------------------------------------------------- engine

ClockEngine::ClockEngine(const Config& cfg) : cfg_(cfg) {
    tracks_[net::kMixerDeviceNumber].isMixer = true;
}

double ClockEngine::alpha(const PlayerTrack& tr) const {
    if (tr.boostBeatsLeft > 0) return 0.7;
    return cfg_.profile == Profile::Stage ? 0.3 : 0.5;
}

double ClockEngine::beta() const { return cfg_.profile == Profile::Stage ? 0.05 : 0.1; }

State ClockEngine::state() const {
    if (master_ == net::kMixerDeviceNumber && state_ == State::Locked) return State::MixerMaster;
    return state_;
}

void ClockEngine::resetTrack(PlayerTrack& tr, int64_t t) {
    tr.phaseNs = t;
    if (tr.reportedBpm > 0) tr.periodNs = 60e9 / tr.reportedBpm;
    if (tr.periodNs <= 0) tr.periodNs = kDefaultPeriodNs;
    tr.hasPhase = true;
    tr.lastAcceptedNs = t;
    tr.residualCount = 0;
    tr.residualPos = 0;
    tr.outlierRun = 0;
    tr.beatsSinceReset = 0;
    tr.pendingJumpCheck = false;
    tr.boostBeatsLeft = 0;
}

BeatOutcome ClockEngine::processBeat(PlayerTrack& tr, int64_t t, bool isMaster) {
    auto recordHit = [&](bool hit) {
        tr.hits[tr.hitPos] = hit ? 1 : 0;
        tr.hitPos = (tr.hitPos + 1) % 8;
        if (tr.hitCount < 8) ++tr.hitCount;
    };
    if (!tr.hasPhase) {
        resetTrack(tr, t);
        tr.beatIndex = 0;
        return BeatOutcome::First;
    }
    if (double(t - tr.lastAcceptedNs) < tr.periodNs / 5.0) return BeatOutcome::Duplicate;

    int64_t k = std::llround(double(t - tr.phaseNs) / tr.periodNs);
    if (k < 1) k = 1;
    const int64_t predicted = tr.phaseNs + int64_t(double(k) * tr.periodNs);
    const double r = double(t - predicted);
    const double gate = std::max(kMinGateNs, 3.0 * tr.mad());
    tr.beatIndex += k;
    ++tr.beatsSinceReset;

    if (std::fabs(r) > gate) {
        const int sign = r > 0 ? 1 : -1;
        const bool sameSign = tr.outlierRun >= 1 && sign == tr.outlierSign;
        const bool allowed = tr.beatsSinceReset >= 4 || !isMaster;
        if ((tr.pendingJumpCheck || sameSign) && allowed) {
            resetTrack(tr, t);
            recordHit(true);
            return BeatOutcome::Reset;
        }
        ++tr.outlierRun;
        tr.outlierSign = sign;
        tr.phaseNs = predicted;
        recordHit(false);
        return BeatOutcome::Coast;
    }

    tr.outlierRun = 0;
    tr.pendingJumpCheck = false;
    // A nudge is an offset clearly above the arrival noise: 10 ms, or 4 MAD when jitter is high.
    // The boost ends early once the residual is back inside the noise, so random jitter
    // that trips the threshold does not keep the fast gain for 8 beats.
    const double nudgeThreshold = std::max(kNudgeNs, 4.0 * tr.mad());
    if (std::fabs(r) > nudgeThreshold) tr.boostBeatsLeft = 8;
    else if (tr.boostBeatsLeft > 0 && std::fabs(r) < nudgeThreshold / 3.0) tr.boostBeatsLeft = 0;
    const double a = alpha(tr);
    if (tr.boostBeatsLeft > 0) --tr.boostBeatsLeft;
    tr.phaseNs = predicted + int64_t(a * r);
    const double periodMeasured = double(t - tr.lastAcceptedNs) / double(k);
    const double periodReported = tr.reportedBpm > 0 ? 60e9 / tr.reportedBpm : periodMeasured;
    tr.periodNs = 0.9 * periodReported + 0.1 * (tr.periodNs + beta() * (periodMeasured - tr.periodNs));
    tr.residualHistory[tr.residualPos] = r;
    tr.residualPos = (tr.residualPos + 1) % 16;
    if (tr.residualCount < 16) ++tr.residualCount;
    tr.lastAcceptedNs = t;
    recordHit(true);
    return BeatOutcome::Accepted;
}

void ClockEngine::updateConfidence(const PlayerTrack& tr) {
    const double rms = tr.rmsResidual() / (8.0 * kMsNs);
    double c = 0.5 * tr.hitRate() + 0.3 * std::exp(-rms * rms) + 0.2 * (state_ == State::Locked ? 1.0 : 0.0);
    tl_.confidence = float(std::clamp(c, 0.0, 1.0));
}

void ClockEngine::publish(const PlayerTrack& tr, uint8_t packetBeatInBar) {
    tl_.bpm = tr.periodNs > 0 ? 60e9 / tr.periodNs : 0;
    tl_.beatOriginNs = tr.phaseNs;
    tl_.beatIndex = tr.beatIndex;
    if (packetBeatInBar >= 1 && packetBeatInBar <= 4)
        tl_.beatInBar = ((packetBeatInBar - 1 + cfg_.userBarOffset) % 4) + 1;
    else
        tl_.beatInBar = (tl_.beatInBar % 4) + 1;
    tl_.barKnown = !(tr.isMixer || tr.model == net::Model::Opus || cfg_.beatOnlyMode ||
                     (tr.statusSeen && tr.trackType != 1) || packetBeatInBar == 0);
    tl_.playing = state_ == State::Locked || state_ == State::Locking || state_ == State::Coasting;
    tl_.masterDevice = master_;
    updateConfidence(tr);
    ++tl_.sequence;
}

void ClockEngine::onMasterChanged(uint8_t from, uint8_t to, int64_t now) {
    (void)from;
    master_ = to;
    if (to == 0) { state_ = State::Idle; tl_.playing = false; tl_.masterDevice = 0; ++tl_.sequence; return; }
    PlayerTrack& tr = tracks_[to];
    tl_.masterDevice = to;
    if (tr.hasPhase) {
        // Warm track: re-phase the timeline onto it (hard re-phase for the outputs).
        tr.beatsSinceReset = 0;
        tr.outlierRun = 0;
        ++tl_.resetSequence;
        state_ = tr.playing ? State::Locked : State::Paused;
        lockBeats_ = 2;
        publish(tr, tr.lastBeatInBar);
    } else {
        state_ = State::Locking;
        lockBeats_ = 0;
        resetOnLock_ = false;
    }
    lastTickNs_ = now;
}

void ClockEngine::onEvent(const net::Event& ev) {
    using net::EventType;
    PlayerTrack& tr = tracks_[ev.device];
    switch (ev.type) {
    case EventType::Beat: {
        tr.present = true;
        const int64_t t = ev.recvTimeNs - cfg_.latencyOffsetNs;
        tr.lastBeatRecvNs = t;
        tr.lastBeatInBar = ev.beat.beatInBar;
        if (!tr.statusSeen && ev.beat.effectiveBpm > 0) tr.reportedBpm = ev.beat.effectiveBpm;
        const bool isMaster = ev.device == master_ && master_ != 0;
        if (isMaster && (state_ == State::Paused || (state_ == State::Idle))) {
            // Resume (or first lock): re-seed the phase silently, then RESET after two beats.
            resetOnLock_ = state_ == State::Paused;
            state_ = State::Locking;
            lockBeats_ = 0;
            tr.hasPhase = false;
        }
        const BeatOutcome out = processBeat(tr, t, isMaster);
        if (!isMaster) return;
        lastOutcome_ = out;
        if (out == BeatOutcome::Duplicate) return;
        if (out == BeatOutcome::Reset) ++tl_.resetSequence;
        if (state_ == State::Locking) {
            if (++lockBeats_ >= 2) {
                if (resetOnLock_) { resetTrack(tr, t); ++tl_.resetSequence; resetOnLock_ = false; }
                state_ = State::Locked;
            }
        } else if (state_ == State::Coasting) {
            state_ = State::Locked;
        }
        tr.playing = tr.statusSeen ? tr.playing : true;
        publish(tr, ev.beat.beatInBar);
        break;
    }
    case EventType::PlayerStatus: {
        const auto& s = ev.status;
        tr.present = true;
        tr.statusSeen = true;
        tr.trackType = s.trackType;
        tr.onAir = s.onAir;
        if (s.bpm100 != 0xffff) tr.reportedBpm = s.bpm100 / 100.0 * s.pitch1;
        if (std::fabs(s.pitch1 - tr.lastPitch1) > 0.0005) tr.boostBeatsLeft = 8;
        tr.lastPitch1 = s.pitch1;
        if (s.beatNumber != 0xffffffff) {
            if (tr.beatNumber != 0xffffffff && tr.hasPhase && tr.periodNs > 0 && s.playing) {
                const double expected = double(tr.beatNumber) + double(ev.recvTimeNs - tr.beatNumberNs) / tr.periodNs;
                if (std::fabs(double(s.beatNumber) - expected) > 1.5) tr.pendingJumpCheck = true;
            }
            tr.beatNumber = s.beatNumber;
            tr.beatNumberNs = ev.recvTimeNs;
        }
        const bool wasPlaying = tr.playing;
        tr.playing = s.playing;
        if (ev.device == master_ && master_ != 0) {
            if (wasPlaying && !s.playing && (state_ == State::Locked || state_ == State::Coasting || state_ == State::Locking)) {
                state_ = State::Paused;
                tl_.playing = false;
                ++tl_.sequence;
            }
        }
        break;
    }
    case EventType::MixerStatus: {
        tr.present = true;
        tr.isMixer = true;
        if (ev.mixer.bpm100 != 0xffff && ev.mixer.bpm100 != 0) {
            tr.reportedBpm = ev.mixer.bpm100 / 100.0;
            tr.statusSeen = true;
        }
        break;
    }
    case EventType::MasterChanged:
        onMasterChanged(ev.masterChanged.from, ev.masterChanged.to, ev.recvTimeNs);
        break;
    case EventType::Device:
        tr.model = ev.deviceEvent.model;
        tr.present = ev.deviceEvent.kind != net::DeviceEvent::Left;
        if (ev.deviceEvent.deviceKind == net::DeviceKind::Mixer) tr.isMixer = true;
        break;
    default:
        break;
    }
}

void ClockEngine::tick(int64_t now) {
    const double dt = lastTickNs_ > 0 ? double(now - lastTickNs_) : 0;
    lastTickNs_ = now;
    if (master_ == 0) return;
    PlayerTrack& tr = tracks_[master_];
    switch (state_) {
    case State::Locked:
        if (tr.hasPhase && tr.playing && double(now - tr.lastAcceptedNs) > 1.5 * tr.periodNs) {
            state_ = State::Coasting;
            coastStartNs_ = now;
        }
        break;
    case State::Coasting:
        if (!tr.playing || now - coastStartNs_ > kCoastToPauseNs) {
            state_ = State::Paused;
            tl_.playing = false;
        }
        tl_.confidence *= float(std::exp(-dt / kConfidenceTauNs));
        ++tl_.sequence;
        break;
    case State::Paused:
        tl_.confidence *= float(std::exp(-dt / kConfidenceTauNs));
        ++tl_.sequence;
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------- publishing policy

bool PublishPolicy::shouldPublishTempo(const Timeline& tl, int64_t nowNs) {
    const bool reset = tl.resetSequence != lastResetSeq_;
    const bool changed = std::fabs(tl.bpm - lastBpm_) > kTempoDelta;
    const bool due = nowNs - lastPublishNs_ >= kTempoIntervalNs;
    if (tl.bpm <= 0) return false;
    if (first_ || reset || (changed && due)) {
        first_ = false;
        lastResetSeq_ = tl.resetSequence;
        lastBpm_ = tl.bpm;
        lastPublishNs_ = nowNs;
        return true;
    }
    return false;
}

PublishPolicy::PhaseAction PublishPolicy::phaseDecision(double phaseErrorBeats, const Timeline& tl) {
    if (tl.resetSequence != phaseResetSeq_) {
        phaseResetSeq_ = tl.resetSequence;
        consecutiveErrors_ = 0;
        return PhaseAction::Hard;
    }
    if (std::fabs(phaseErrorBeats) > kPhaseErrorBeats) {
        if (++consecutiveErrors_ >= 2) return PhaseAction::Soft;
    } else {
        consecutiveErrors_ = 0;
    }
    return PhaseAction::None;
}

} // namespace shunt::clock
