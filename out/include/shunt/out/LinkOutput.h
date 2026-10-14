// Link output adapter (ES-05 section 4). The Ableton Link SDK sits behind ILinkBackend.
#pragma once
#include "shunt/clock/ClockEngine.h"
#include "shunt/out/OutputStatus.h"
#include <cstdint>

namespace shunt::out {

class ILinkBackend {
public:
    virtual ~ILinkBackend() = default;
    virtual void setTempo(double bpm, int64_t atNs) = 0;
    virtual double beatAtTime(int64_t ns, double quantum) const = 0;
    virtual void requestBeatAtTime(double beat, int64_t ns, double quantum) = 0;
    virtual void forceBeatAtTime(double beat, int64_t ns, double quantum) = 0;
    virtual void setPlaying(bool playing, int64_t ns) = 0;
    virtual size_t numPeers() const = 0;
};

struct LinkConfig {
    bool syncStartStop = false;
};

class LinkOutput {
public:
    LinkOutput(ILinkBackend& backend, const LinkConfig& cfg = {}) : link_(backend), cfg_(cfg) {}

    void update(const clock::Timeline& tl, int64_t nowNs);
    const OutputStatus& status() const { return status_; }
    static double quantumFor(const clock::Timeline& tl) { return tl.barKnown ? 4.0 : 1.0; }

private:
    ILinkBackend& link_;
    LinkConfig cfg_;
    OutputStatus status_;
    clock::PublishPolicy policy_;
    int64_t lastBeat_ = INT64_MIN;
    int lastPlaying_ = -1;
};

} // namespace shunt::out
