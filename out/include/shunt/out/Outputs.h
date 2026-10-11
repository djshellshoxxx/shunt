// Clock outputs (RS-01; Pacemaker docs/specs/ES-02-clock-outputs.md for shared
// behaviour): Ableton Link, MIDI clock, OSC. Plain C++17; the MIDI port itself is
// behind IMidiSink so the JUCE apps, the Box UART and tests plug in their own.
#pragma once
#include "shunt/out/TimelineBuffer.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace shunt::out {

// ---------------------------------------------------------------- status

struct OutputStatus {
    std::string id;            // "link", "midi:<port>", "osc"
    std::string label;         // human readable
    bool enabled = false;
    bool connected = false;    // Link: peers > 0; MIDI: port open; OSC: socket ok
    std::string detail;        // "3 peers", "Elektron Digitakt", "192.168.1.20:7000"
    std::string lastError;
    double jitterMsRms = 0;    // measured scheduling jitter
    int peers = 0;             // Link only
};

// ---------------------------------------------------------------- MIDI

// A MIDI destination. send() is called from the clock thread with the monotonic
// time (monotonicNowNs clock) the bytes are due; sinks that support timestamps
// may schedule, others send immediately.
class IMidiSink {
public:
    virtual ~IMidiSink() = default;
    virtual bool isOpen() const = 0;
    virtual std::string name() const = 0;
    virtual void send(const uint8_t* bytes, size_t len, int64_t dueNs) = 0;
};

struct MidiPortInfo {
    std::string id;            // stable identifier for settings
    std::string name;          // display name
    bool isVirtual = false;
};

// Supplied by the host application: enumerates and opens ports.
class IMidiSinkFactory {
public:
    virtual ~IMidiSinkFactory() = default;
    virtual std::vector<MidiPortInfo> ports() = 0;
    virtual std::unique_ptr<IMidiSink> open(const std::string& portId) = 0;
    // Virtual port "Shunt MIDI" (macOS, Linux); nullptr where unsupported (Windows).
    virtual std::unique_ptr<IMidiSink> openVirtual(const std::string& /*name*/) { return nullptr; }
};

enum class PauseBehaviour : uint8_t { KeepRunning, Stop };

struct MidiClockConfig {
    bool enabled = false;
    std::string portId;                    // from IMidiSinkFactory::ports(), or "virtual"
    PauseBehaviour onPause = PauseBehaviour::KeepRunning;
    bool startOnReset = false;             // send Start on resetSequence change (off: hardware restarts patterns)
    bool sppOnReset = false;               // Song Position Pointer before Continue
    bool muteWhenOffAir = false;           // P1: stop clock while the master is off air
    int64_t latencyOffsetNs = 0;           // per-output offset, -20 ms .. +50 ms
};

// ---------------------------------------------------------------- Link

struct LinkConfig {
    bool enabled = false;
    bool startStopSync = false;            // Link start/stop state follows play/pause
    int64_t latencyOffsetNs = 0;
};

// ---------------------------------------------------------------- OSC

enum class OscProfile : uint8_t { Generic, Resolume, GrandMA3, MagicQ, QLab };

struct OscConfig {
    bool enabled = false;
    std::string host = "127.0.0.1";
    uint16_t port = 7000;
    OscProfile profile = OscProfile::Generic;
    bool sendBeats = true;                 // /shunt/beat i beatInBar on every beat
    int qlabMasterCue = 0, qlabTrackCue = 0;   // QLab: user-mapped cue numbers (0 = off)
    int64_t latencyOffsetNs = 0;
};

struct OutputsConfig {
    LinkConfig link;
    std::vector<MidiClockConfig> midi;     // one entry per MIDI clock destination
    OscConfig osc;
};

// Non-clock facts the outputs forward (OSC extras, mute on off-air).
struct OutputEvent {
    enum Kind : uint8_t { MasterChanged, TrackLoaded, OnAir, TrackId } kind;
    uint8_t deck = 0;          // or master device for MasterChanged
    int state = 0;             // OnAir: 1/0
    uint32_t rekordboxId = 0;
    std::string title, artist;
};

// Owns the output threads. configure() starts and stops outputs to match the
// config and may be called at any time from the control thread.
class OutputHub {
public:
    OutputHub(TimelineBuffer& timeline, IMidiSinkFactory* midiFactory /* may be null */);
    ~OutputHub();
    OutputHub(const OutputHub&) = delete;
    OutputHub& operator=(const OutputHub&) = delete;

    void configure(const OutputsConfig& cfg);
    void post(const OutputEvent& ev);      // thread-safe
    void setMasterOnAir(bool onAir);       // drives muteWhenOffAir
    std::vector<OutputStatus> status() const;
    // Phase scope: last residuals (ms) of beat output vs. timeline, newest last.
    std::vector<double> phaseResidualsMs() const;
    void stopAll();

    static bool linkAvailable();           // false when built without SHUNT_WITH_LINK

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

const char* oscProfileName(OscProfile p);
OscProfile oscProfileFromName(const std::string& s);

} // namespace shunt::out
