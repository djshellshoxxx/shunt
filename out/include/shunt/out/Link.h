// Ableton Link output logic (RS-07 section 4) over an abstract session.
#pragma once
#include "shunt/clock/ClockEngine.h"
#include <memory>
#include <string>

namespace shunt::out {

class ILinkSession {
public:
    virtual ~ILinkSession() = default;
    virtual void enable(bool on) = 0;
    virtual size_t numPeers() const = 0;
    virtual void setTempo(double bpm, int64_t atNs) = 0;
    virtual double beatAtTime(int64_t timeNs, double quantum) const = 0;
    virtual void requestBeatAtTime(double beat, int64_t timeNs, double quantum) = 0;
    virtual void forceBeatAtTime(double beat, int64_t timeNs, double quantum) = 0;
};

// Returns nullptr when built without -DSHUNT_WITH_LINK.
std::unique_ptr<ILinkSession> makeLinkSdkSession();

class LinkOutput {
public:
    explicit LinkOutput(std::unique_ptr<ILinkSession> s = nullptr) : session_(std::move(s)) {}
    void setSession(std::unique_ptr<ILinkSession> s) { session_ = std::move(s); }
    void setEnabled(bool on);
    bool enabled() const { return enabled_; }
    bool available() const { return session_ != nullptr; }
    size_t peers() const { return session_ ? session_->numPeers() : 0; }
    std::string error() const { return session_ ? "" : "Link SDK not built in"; }
    double lastQuantum() const { return quantum_; }
    uint64_t softCorrections() const { return soft_; }
    uint64_t hardCorrections() const { return hard_; }

    void poll(int64_t nowNs, const clock::Timeline& tl);

private:
    std::unique_ptr<ILinkSession> session_;
    clock::PublishPolicy policy_;
    bool enabled_ = false, haveBeat_ = false;
    int64_t nextBeat_ = 0;
    double quantum_ = 4;
    uint64_t soft_ = 0, hard_ = 0;
};

} // namespace shunt::out
