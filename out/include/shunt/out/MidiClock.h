// MIDI clock generator (ES-05 section 2): Timeline -> timestamped realtime bytes.
#pragma once
#include "shunt/clock/ClockEngine.h"
#include "shunt/out/OutputStatus.h"
#include <cstddef>
#include <cstdint>

namespace shunt::out {

class IMidiSink {
public:
    virtual ~IMidiSink() = default;
    // atNs is an absolute monotonic timestamp; bytes are 1 to 3 MIDI bytes.
    virtual void send(int64_t atNs, const uint8_t* bytes, size_t len) = 0;
};

enum class PauseMode : uint8_t { KeepRunning, Stop };

struct MidiClockConfig {
    PauseMode pauseMode = PauseMode::KeepRunning;
    bool startOnReset = false;
    bool sppOnReset = false;
    int64_t horizonNs = 20'000'000;
};

class MidiClockGenerator {
public:
    static constexpr int kTicksPerBeat = 24;

    explicit MidiClockGenerator(const MidiClockConfig& cfg = {}) : cfg_(cfg) {}

    void update(const clock::Timeline& tl, int64_t nowNs, IMidiSink& sink);
    void setConfig(const MidiClockConfig& c) { cfg_ = c; }
    const OutputStatus& status() const { return status_; }

private:
    enum class Pending : uint8_t { None, Start, Continue, SppContinue };

    void emit(IMidiSink& sink, int64_t t, uint8_t b0, int n = 1, uint8_t b1 = 0, uint8_t b2 = 0);
    int64_t tickTime(const clock::Timeline& tl, int64_t beat, int sub) const;

    MidiClockConfig cfg_;
    OutputStatus status_;
    bool everPlaying_ = false;
    bool started_ = false;       // Start sent at least once
    bool stopped_ = false;       // Stop sent, awaiting resume
    bool haveLast_ = false;
    int64_t lastTickNs_ = 0;
    uint32_t seenReset_ = 0;
    bool resetSeen_ = false;
    Pending pending_ = Pending::None;
};

} // namespace shunt::out
