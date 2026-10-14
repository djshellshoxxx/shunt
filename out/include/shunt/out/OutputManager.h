// Owns Link, MIDI clock and OSC outputs and the 1 ms output thread (RS-07 section 5).
#pragma once
#include "shunt/out/Link.h"
#include "shunt/out/Midi.h"
#include "shunt/out/Osc.h"
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace shunt::out {

struct LinkSettings { bool enabled = false; };
struct MidiSettings {
    bool enabled = false;
    std::string port;
    MidiClockOptions options;
};
struct OutputSettings { LinkSettings link; MidiSettings midi; OscSettings osc; };

struct OutputStatus {
    bool enabled = false, available = true, connected = false;
    std::string error, detail;
    double peers = 0, jitterUs = 0;
    uint64_t sent = 0;
};
struct OutputsStatus { OutputStatus link, midi, osc; };

// Event notifications from the net thread (master change, track load, on-air).
struct OutputEvent {
    enum Kind { Master, Track, OnAir } kind;
    int deck = 0;
    uint32_t id = 0;
    bool on = false;
    std::string title, artist;
};

class OutputManager {
public:
    using TimelineSource = std::function<clock::Timeline()>;
    explicit OutputManager(TimelineSource src);
    ~OutputManager();

    void configure(const OutputSettings& s);
    void start();
    void stop();
    void push(const OutputEvent& e);
    OutputsStatus status() const;
    // Test hook: replace the MIDI port.
    void setMidiPort(std::unique_ptr<IMidiPort> p);
    void setLinkSession(std::unique_ptr<ILinkSession> s);
    OscOutput& osc() { return osc_; }

private:
    void run();
    void openMidi();

    TimelineSource src_;
    mutable std::mutex mu_;
    OutputSettings settings_;
    std::unique_ptr<IMidiPort> port_;
    bool portOverride_ = false;
    std::string midiError_;
    MidiClockGenerator gen_;
    LinkOutput link_;
    OscOutput osc_;
    std::deque<OutputEvent> events_;
    std::deque<double> jitter_;
    uint64_t midiSent_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace shunt::out
