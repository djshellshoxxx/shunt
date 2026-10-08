// Core runtime (RS-07 section 7): net thread, clock engine, tracklist, outputs, status snapshot.
#pragma once
#include "shunt/app/Json.h"
#include "shunt/app/Settings.h"
#include "shunt/clock/ClockEngine.h"
#include "shunt/log/Tracklist.h"
#include "shunt/net/NetworkStack.h"
#include "shunt/out/OutputManager.h"
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace shunt::app {

class Core {
public:
    Core(Settings s, std::string configPath, std::string dataDir);
    ~Core();
    bool start(std::string* error = nullptr);
    void stop();

    Json status();
    Json config();
    bool applyConfig(const Json& patch, std::string& error);
    Json interfaces();
    Json midiPorts();
    Json tracklist();
    Json compat();
    // format: csv | cue | txt | ndjson | report. Returns false for an unknown format.
    bool exportTracklist(const std::string& format, std::string& body, std::string& contentType, std::string& filename);

    void setBarOffset(int offset);
    void cycleBarOffset();
    void setBeatOnly(bool on);
    void setLatencyMs(double ms);
    bool latencyFromScope();
    void markSetStart();
    void clearSession();
    void setRecording(bool on);

    const Settings& settings() const { return settings_; }

private:
    struct Live {
        double bpm = 0, pitchPct = 0;
        bool playing = false, onAir = false, master = false;
        uint32_t trackId = 0;
        uint8_t slot = 0, trackType = 0;
        int beatInBar = 0;
        int64_t lastBeatNs = 0;
    };
    void netLoop();
    void buildStack();
    void handle(const net::Event& ev, int64_t wallMs);
    void applyEngineConfig();
    void persistLocked();
    void appendTracklistFilesLocked();
    clock::Timeline timeline();

    Settings settings_;
    std::string configPath_, dataDir_;
    std::mutex mu_;
    std::unique_ptr<net::ISocketFactory> factory_;
    std::unique_ptr<net::NetworkStack> stack_;
    clock::ClockEngine engine_;
    log::TracklistSession session_;
    out::OutputManager outputs_;
    std::map<uint8_t, Live> live_;
    std::deque<double> scope_;
    clock::Timeline tl_;
    std::vector<net::Device> devices_;
    net::Counters counters_{};
    std::string timestampSource_, stackError_, ifaceInUse_, claimState_;
    int claimNumber_ = 0;
    bool firstRun_ = false, recording_ = false, stackUp_ = false;
    std::string capturePath_;
    int64_t setStartMs_ = 0, startNs_ = 0;
    uint32_t masterTrackId_ = 0;
    uint8_t masterTrackSlot_ = 0;
    size_t writtenEvents_ = 0, writtenRows_ = 0;
    std::string sessionFile_;
    std::atomic<bool> running_{false}, restart_{false};
    std::thread thread_;
};

} // namespace shunt::app
