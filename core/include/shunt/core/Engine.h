// The Shunt engine: network stack -> clock engine -> outputs, plus tracklist,
// metadata, bar offset memory, OSC input and recording/replay. Shared by the
// desktop app and shunt_headless; no JUCE. All public methods are thread-safe
// and may be called from the UI / web threads.
#pragma once
#include "shunt/core/Compatibility.h"
#include "shunt/core/Licence.h"
#include "shunt/core/Settings.h"
#include "shunt/log/Tracklist.h"
#include "shunt/net/NetworkStack.h"
#include "shunt/out/Outputs.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace shunt::core {

struct DeviceView {
    uint8_t number = 0;
    std::string name;
    net::Model model = net::Model::Unknown;
    std::string modelName;
    Badge badge = Badge::Unverified;
    bool isMixer = false;
    bool playing = false, master = false, onAir = false, synced = false;
    double bpm = 0;            // effective (bpm * pitch)
    double pitchPercent = 0;
    int beatInBar = 0;
    uint32_t rekordboxId = 0;
    uint8_t slot = 0, trackType = 0;
    std::string title, artist;  // when resolved
    uint64_t packets = 0;
    double lastSeenS = 0;       // seconds since last packet
};

struct TracklistView {
    std::vector<log::Row> rows;     // played rows so far, plus the open rows
    int64_t setStartMs = 0;         // "set start" marker (wall clock ms)
    std::string sessionFile;        // NDJSON being written
};

struct Snapshot {
    // engine
    bool running = false;
    std::string interfaceName, interfaceAddress;
    net::Mode mode = net::Mode::Follow;
    std::string claimState;         // "Watching", "Claiming", "Active 7", "Passive", "Failed: ..."
    uint8_t ownNumber = 0;
    std::string timestampSource;
    // clock
    clock::Timeline timeline;
    clock::State clockState = clock::State::Idle;
    std::string clockStateName;
    uint8_t master = 0;
    std::string masterName;
    int userBarOffset = 0;
    bool beatOnly = false;
    clock::Profile profile = clock::Profile::Stage;
    int64_t latencyOffsetNs = 0;
    // phase scope
    std::vector<double> residualsMs;    // last 64
    double jitterRmsMs = 0;
    // devices, outputs, tracklist
    std::vector<DeviceView> devices;
    std::vector<out::OutputStatus> outputs;
    TracklistView tracklist;
    // diagnostics
    net::Counters counters{};
    bool recording = false;
    std::string recordingPath;
    bool replaying = false;
    std::string replayPath;
    // licence
    Edition edition = Edition::Monitor;
    bool outputsAllowed = true;
    std::string lastError;
};

struct InterfaceChoice {
    std::string name, address;
    bool seesPlayers = false;       // keep-alives seen during the probe
    bool loopback = false;
};

class Engine {
public:
    explicit Engine(out::IMidiSinkFactory* midiFactory = nullptr);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Applies settings (restarts the network if interface or mode changed).
    void applySettings(const Settings& s);
    Settings settings() const;
    void setSettingsPath(const std::string& path);    // persist on every change when set
    void setLicence(const Licence& l);

    bool start();                   // network thread; false if the sockets cannot bind
    void stop();
    bool isRunning() const;

    Snapshot snapshot() const;
    std::string stateJson() const;  // WebSocket / overlay JSON (see docs/specs/RS-07-state-json.md)

    // controls (ES-04 section 5)
    void setBarOffset(int offset);  // 0..3; stored in bar offset memory for the loaded master track
    void cycleBarOffset();
    void setBeatOnly(bool on);
    void setMode(net::Mode m);
    void setLatencyOffsetNs(int64_t ns);
    void setProfile(clock::Profile p);
    void setOutputs(const out::OutputsConfig& c);

    // tracklist
    void markSetStart();
    std::string exportTracklist(const std::string& format) const;   // csv, cue, txt, ndjson, report
    bool exportTracklistToFile(const std::string& format, const std::string& path) const;
    bool loadRekordboxXml(const std::string& path, std::string* error = nullptr);

    // record and replay (pcapng)
    bool startRecording(const std::string& path = "");   // empty: dataDir/captures/<timestamp>.pcapng
    void stopRecording();
    bool startReplay(const std::string& path, double speed = 1.0);   // stops live network while replaying
    void stopReplay();

    // first run: probe interfaces for player keep-alives (blocks up to timeoutMs)
    static std::vector<InterfaceChoice> probeInterfaces(int timeoutMs = 3000);
    static bool rekordboxRunningOnHost();       // suggest Passive mode

    // OSC / MIDI learn style control by id: "baroffset", "beatonly", "follow", "setstart", "record"
    void control(const std::string& id, int value);

    // Notified on the engine thread after notable changes (master, device, row).
    void setChangeListener(std::function<void()> fn);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

const char* clockStateName(clock::State s);
const char* modeName(net::Mode m);

} // namespace shunt::core
