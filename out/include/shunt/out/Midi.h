// MIDI clock output (RS-07 section 2).
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

class MidiClockGenerator {
public:
    void setOptions(const MidiClockOptions& o) { opt_ = o; }
    // Appends every message due before nowNs + lookaheadNs.
    void poll(int64_t nowNs, int64_t lookaheadNs, const clock::Timeline& tl, std::vector<MidiMsg>& out);
    bool running() const { return locked_ && !stopped_; }

private:
    double tickTimeNs(int64_t n, const clock::Timeline& tl) const;
    int beatInBarOf(int64_t beat, const clock::Timeline& tl) const;
    void resync(int64_t nowNs, const clock::Timeline& tl);
    MidiClockOptions opt_;
    bool locked_ = false, stopped_ = false, pendingStart_ = false, pendingContinue_ = false;
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
