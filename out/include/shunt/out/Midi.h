// MIDI clock output (RS-07 section 2). Mute (FS-01): setMuted/setUnmuteMode.
#pragma once
#include "shunt/clock/ClockEngine.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace shunt::out {

struct MidiMsg {
    int64_t timeNs = 0;
    uint8_t bytes[3] = {0, 0, 0};
    uint8_t len = 1;
};

struct MidiClockOptions {
    enum Pause : uint8_t { KeepRunning, Stop } pause = KeepRunning;
    bool startOnReset = false, sppOnReset = false, startOnFirstLock = true;
};

// How the clock restarts after an on-air mute (FS-01).
enum class UnmuteMode : uint8_t { Start, Continue };

class MidiClockGenerator {
public:
    void setOptions(const MidiClockOptions& o) { opt_ = o; }
    // Appends every message due before nowNs + lookaheadNs.
    void poll(int64_t nowNs, int64_t lookaheadNs, const clock::Timeline& tl, std::vector<MidiMsg>& out);
    bool running() const { return locked_ && !stopped_ && !muteApplied_; }
    // FS-01 on-air mute. Muted wins over the pause logic: the next poll sends Stop (0xFC) once
    // (if the clock was running) and no further messages. Unmuting resyncs and sends Start or
    // Continue (setUnmuteMode) at the next bar boundary (next beat when the bar is unknown).
    void setMuted(bool m) { muted_ = m; }
    void setUnmuteMode(UnmuteMode m) { unmuteMode_ = m; }
    bool muted() const { return muteApplied_; }

private:
    double tickTimeNs(int64_t n, const clock::Timeline& tl) const;
    int beatInBarOf(int64_t beat, const clock::Timeline& tl) const;
    void resync(int64_t nowNs, const clock::Timeline& tl);
    MidiClockOptions opt_;
    bool locked_ = false, stopped_ = false, pendingStart_ = false, pendingContinue_ = false;
    bool muted_ = false, muteApplied_ = false, pendingUnmute_ = false, resumeAfterMute_ = false, alignBar_ = false;
    UnmuteMode unmuteMode_ = UnmuteMode::Start;
    int64_t nextTick_ = 0;
    int64_t lastEmitNs_ = 0;
    uint32_t lastReset_ = 0;
};

class IMidiPort {
public:
    virtual ~IMidiPort() = default;
    virtual bool write(const uint8_t* data, size_t len) = 0;
    virtual std::string name() const = 0;
};

class MemoryMidiPort : public IMidiPort {
public:
    bool write(const uint8_t* d, size_t n) override { bytes.insert(bytes.end(), d, d + n); return true; }
    std::string name() const override { return "memory"; }
    std::vector<uint8_t> bytes;
};

// ALSA raw MIDI device node or serial device (Linux).
std::unique_ptr<IMidiPort> openRawMidiPort(const std::string& path, std::string& error);

struct MidiPortInfo { std::string id, name; };
std::vector<MidiPortInfo> listMidiPorts();

} // namespace shunt::out
