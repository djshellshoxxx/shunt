#include "shunt/out/LinkOutput.h"
#include <cmath>

namespace shunt::out {

using clock::PublishPolicy;

void LinkOutput::update(const clock::Timeline& tl, int64_t nowNs) {
    status_.enabled = true;
    status_.connected = true;
    if (!usable(tl)) return;
    const double q = quantumFor(tl);

    if (policy_.shouldPublishTempo(tl, nowNs)) {
        link_.setTempo(tl.bpm, nowNs);
        ++status_.messagesSent;
    }
    if (cfg_.syncStartStop && int(tl.playing) != lastPlaying_) {
        lastPlaying_ = int(tl.playing);
        link_.setPlaying(tl.playing, nowNs);
    }
    if (!tl.playing) return;

    // Once per beat: compare Link's phase with Shunt's beat position (mod quantum).
    const double period = tl.periodNs();
    int64_t k = tl.beatIndex + int64_t(std::floor(double(nowNs - tl.beatOriginNs) / period));
    if (tl.timeOfBeat(k) > nowNs) --k;
    if (k == lastBeat_) return;
    lastBeat_ = k;

    const int bib = ((tl.beatInBar - 1 + int(k - tl.beatIndex)) % 4 + 4) % 4;   // 0..3
    const double frac = double(nowNs - tl.timeOfBeat(k)) / period;
    const double expectedNow = std::fmod((tl.barKnown ? double(bib) : 0.0) + frac, q);
    const double raw = link_.beatAtTime(nowNs, q);
    double err = std::fmod(raw, q) - expectedNow;
    err -= q * std::round(err / q);                                              // wrap to +-q/2

    const double target = raw - err;                                             // Link beat that matches Shunt now
    switch (policy_.phaseDecision(err, tl)) {
    case PublishPolicy::PhaseAction::Hard: link_.forceBeatAtTime(target, nowNs, q); ++status_.messagesSent; break;
    case PublishPolicy::PhaseAction::Soft: link_.requestBeatAtTime(target, nowNs, q); ++status_.messagesSent; break;
    case PublishPolicy::PhaseAction::None: break;
    }
}

} // namespace shunt::out
