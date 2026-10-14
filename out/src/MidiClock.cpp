#include "shunt/out/MidiClock.h"
#include <cmath>

namespace shunt::out {

using clock::Timeline;

void MidiClockGenerator::emit(IMidiSink& sink, int64_t t, uint8_t b0, int n, uint8_t b1, uint8_t b2) {
    const uint8_t bytes[3] = {b0, b1, b2};
    sink.send(t, bytes, size_t(n));
    ++status_.messagesSent;
}

int64_t MidiClockGenerator::tickTime(const Timeline& tl, int64_t beat, int sub) const {
    const int64_t t0 = tl.timeOfBeat(beat);
    return t0 + int64_t(double(sub) * tl.periodNs() / kTicksPerBeat);
}

void MidiClockGenerator::update(const Timeline& tl, int64_t nowNs, IMidiSink& sink) {
    status_.enabled = true;
    status_.connected = true;
    if (!usable(tl)) return;
    if (tl.playing) everPlaying_ = true;
    if (!everPlaying_) return;

    // Transport state changes decided from the timeline.
    if (!tl.playing && cfg_.pauseMode == PauseMode::Stop && started_ && !stopped_) {
        emit(sink, nowNs, 0xFC);
        stopped_ = true;
        pending_ = Pending::None;
    }
    if (tl.playing && stopped_) {
        stopped_ = false;
        pending_ = Pending::Continue;
    }
    if (!started_) pending_ = Pending::Start;
    if (resetSeen_ && tl.resetSequence != seenReset_ && started_ && !stopped_) {
        if (cfg_.sppOnReset) pending_ = Pending::SppContinue;
        else if (cfg_.startOnReset) pending_ = Pending::Start;
    }
    resetSeen_ = true;
    seenReset_ = tl.resetSequence;

    if (stopped_ || (!tl.playing && cfg_.pauseMode == PauseMode::Stop)) return;

    const double tickNs = tl.periodNs() / kTicksPerBeat;
    const int64_t horizon = nowNs + cfg_.horizonNs;
    // First tick time not earlier than now and at least half a tick after the last emitted.
    int64_t earliest = nowNs;
    if (haveLast_ && lastTickNs_ + int64_t(tickNs / 2) > earliest) earliest = lastTickNs_ + int64_t(tickNs / 2);
    const double pos = double(earliest - tl.beatOriginNs) / tickNs;
    int64_t n = int64_t(std::ceil(pos - 1e-9));
    const int64_t baseTick = tl.beatIndex * kTicksPerBeat;

    for (;; ++n) {
        const int64_t abs = baseTick + n;
        int64_t beat = abs / kTicksPerBeat;
        int sub = int(abs % kTicksPerBeat);
        if (sub < 0) { sub += kTicksPerBeat; --beat; }
        const int64_t t = tickTime(tl, beat, sub);
        if (t > horizon) break;
        if (t < earliest) continue;
        if (sub == 0 && pending_ != Pending::None) {
            switch (pending_) {
            case Pending::Start:
                emit(sink, t, 0xFA);
                started_ = true;
                break;
            case Pending::Continue:
                emit(sink, t, 0xFB);
                break;
            case Pending::SppContinue: {
                int bib = tl.barKnown ? ((tl.beatInBar - 1 + int(beat - tl.beatIndex)) % 4 + 4) % 4 : 0;
                const int spp = bib * 4;
                emit(sink, t, 0xF2, 3, uint8_t(spp & 0x7f), uint8_t((spp >> 7) & 0x7f));
                emit(sink, t, 0xFB);
                break;
            }
            case Pending::None: break;
            }
            pending_ = Pending::None;
        } else if (pending_ == Pending::Start && !started_) {
            continue;   // hold clock until the Start beat so the first tick coincides with Start
        }
        emit(sink, t, 0xF8);
        lastTickNs_ = t;
        haveLast_ = true;
    }
}

} // namespace shunt::out
