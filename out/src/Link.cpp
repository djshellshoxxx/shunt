#include "shunt/out/Link.h"
#include <cmath>
#ifdef SHUNT_WITH_LINK
#include <ableton/Link.hpp>
#include <chrono>
#endif

namespace shunt::out {

void LinkOutput::setEnabled(bool on) {
    enabled_ = on;
    haveBeat_ = false;
    policy_ = clock::PublishPolicy{};
    if (session_) session_->enable(on);
}

void LinkOutput::poll(int64_t nowNs, const clock::Timeline& tl) {
    if (!enabled_ || !session_ || tl.bpm <= 0) return;
    quantum_ = tl.barKnown ? 4.0 : 1.0;
    if (policy_.shouldPublishTempo(tl, nowNs)) session_->setTempo(tl.bpm, nowNs);
    if (!haveBeat_) {
        const double pos = double(tl.beatIndex) + double(nowNs - tl.beatOriginNs) / tl.periodNs();
        nextBeat_ = int64_t(std::floor(pos)) + 1;
        haveBeat_ = true;
    }
    int guard = 0;
    while (guard++ < 8 && nowNs >= tl.timeOfBeat(nextBeat_)) {
        const int64_t tBeat = tl.timeOfBeat(nextBeat_);
        const int inBar = tl.barKnown ? int(((int64_t(tl.beatInBar - 1) + (nextBeat_ - tl.beatIndex)) % 4 + 4) % 4) : 0;
        const double target = double(inBar);
        double err = std::fmod(session_->beatAtTime(tBeat, quantum_) - target, quantum_);
        if (err > quantum_ / 2) err -= quantum_;
        if (err <= -quantum_ / 2) err += quantum_;
        switch (policy_.phaseDecision(err, tl)) {
        case clock::PublishPolicy::PhaseAction::Soft: session_->requestBeatAtTime(target, tBeat, quantum_); ++soft_; break;
        case clock::PublishPolicy::PhaseAction::Hard: session_->forceBeatAtTime(target, tBeat, quantum_); ++hard_; break;
        default: break;
        }
        ++nextBeat_;
    }
}

#ifdef SHUNT_WITH_LINK
// Binding to the Ableton Link SDK; built only when the SDK is available and licensed.
namespace {
class SdkSession : public ILinkSession {
public:
    SdkSession() : link_(120.0) {}
    void enable(bool on) override { link_.enable(on); }
    size_t numPeers() const override { return link_.numPeers(); }
    void setTempo(double bpm, int64_t ns) override { auto s = link_.captureAppSessionState(); s.setTempo(bpm, us(ns)); link_.commitAppSessionState(s); }
    double beatAtTime(int64_t ns, double q) const override { return link_.captureAppSessionState().beatAtTime(us(ns), q); }
    void requestBeatAtTime(double b, int64_t ns, double q) override { auto s = link_.captureAppSessionState(); s.requestBeatAtTime(b, us(ns), q); link_.commitAppSessionState(s); }
    void forceBeatAtTime(double b, int64_t ns, double q) override { auto s = link_.captureAppSessionState(); s.forceBeatAtTime(b, us(ns), q); link_.commitAppSessionState(s); }
private:
    static std::chrono::microseconds us(int64_t ns) { return std::chrono::microseconds(ns / 1000); }
    mutable ableton::Link link_;
};
}
std::unique_ptr<ILinkSession> makeLinkSdkSession() { return std::make_unique<SdkSession>(); }
#else
std::unique_ptr<ILinkSession> makeLinkSdkSession() { return nullptr; }
#endif

} // namespace shunt::out
