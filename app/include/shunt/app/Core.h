// Core runtime (RS-07 section 7): net thread, clock engine, tracklist, outputs, status snapshot.
// Feature host, control registry and replay seam: FS-00 (docs/specs/FS-00-extension-points.md).
#pragma once
#include "shunt/app/ControlRegistry.h"
#include "shunt/app/Feature.h"
#include "shunt/app/Json.h"
#include "shunt/app/MetaStore.h"
#include "shunt/app/Settings.h"
#include "shunt/clock/ClockEngine.h"
#include "shunt/log/RekordboxXml.h"
#include "shunt/meta/Metadata.h"
#include "shunt/log/Tracklist.h"
#include "shunt/net/NetworkStack.h"
#include "shunt/out/OutputManager.h"
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
    // Imports a rekordbox XML export; returns the number of tracks with metadata, or -1 when none were found.
    int importRekordboxXml(const std::string& xml);

    const Settings& settings() const { return settings_; }

    // ---- FS-00 extension points ----
    // Adds a feature next to the registered ones (tests). Only before the first start(); false otherwise.
    bool addFeature(std::unique_ptr<IFeature> f);
    Json features();                                   // [{id, enabled, error}] in id order
    // Routes /api/f/<id>/<subpath>. nullopt: unknown id or the feature declined (404).
    std::optional<HttpResponse> featureRequest(const std::string& id, const std::string& subpath, const HttpRequest& req);
    ControlRegistry& controls() { return controls_; }
    Json controlsList();                               // [{id, label, kind, group}]
    MetaStore& meta() { return meta_; }

private:
    class HostImpl;
    struct FeatureSlot;
    struct Parked;
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
    void pushOutputLocked(const out::OutputEvent& e);
    // FS-00 feature host (CoreFeatures.cpp)
    void registerControls();
    void createFeatures();
    void shutdownFeatures();
    void featuresOnEventLocked(const net::Event& ev, int64_t wallMs);
    void featuresOnTickLocked(int64_t nowNs, int64_t wallMs);
    Json featuresStatusLocked();
    void featureFailed(FeatureSlot& f, const char* where, const std::string& what);
    std::vector<std::pair<FeatureSlot*, Json>> syncFeatureSettingsLocked(const Json& before);
    bool featureEnabledLocked(const FeatureSlot& f) const;
    bool enterReplay(ReplayHooks* hooks);
    void leaveReplay();
    void leaveReplayLocked();
    bool feedLocked(uint16_t port, const uint8_t* data, size_t len, int64_t recvNs, uint32_t srcIp, int64_t wallMs);
    void resetEngineLocked(bool clearSession);
    void replayPassLocked(int64_t nowNs, int64_t wallMs);
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
    MetaStore meta_;               // own leaf lock (FS-00 section 4)
    meta::DbServerClient db_;      // dbserver lookups; callbacks run on its worker thread
    out::OscInput oscIn_;
    int oscInPortOpen_ = 0;
    void syncOscInput();
    void onOsc(const out::OscMessage& m);

    ControlRegistry controls_;
    std::unique_ptr<HostImpl> host_;
    std::vector<std::unique_ptr<IFeature>> extraFeatures_;   // from addFeature(), before start
    std::vector<std::unique_ptr<FeatureSlot>> features_;     // immutable once featuresReady_
    std::atomic<bool> featuresReady_{false};
    bool featuresCreated_ = false;
    std::mutex cbMu_;                                        // leaf: session-cleared callbacks
    std::vector<std::function<void()>> sessionCleared_;
    // replay seam (FS-00 section 1, FS-05); guarded by mu_
    ReplayHooks* replay_ = nullptr;
    std::unique_ptr<net::NetworkStack> replayStack_;
    std::unique_ptr<Parked> parked_;
    bool outputsEnabled_ = true, leaveReplayRequested_ = false;
    std::atomic<bool> running_{false}, restart_{false};
    std::thread thread_;
};

} // namespace shunt::app
