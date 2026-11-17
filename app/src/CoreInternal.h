// Private Core types shared by Core.cpp and CoreFeatures.cpp (FS-00). Original Shunt code.
#pragma once
#include "shunt/app/Core.h"
#include <deque>

namespace shunt::app {

struct Core::FeatureSlot {
    std::unique_ptr<IFeature> f;
    std::string id;
    bool enabled = false;                // guarded by Core::mu_
    std::atomic<bool> failed{false};     // error state: 5 exceptions within 60 s
    std::mutex errMu;                    // leaf lock
    std::deque<int64_t> errNs;
    std::string lastError;
    uint64_t errors = 0;
};

// Live state parked while a replay runs (FS-00 replay seam).
struct Core::Parked {
    clock::ClockEngine engine;
    log::TracklistSession session;
    std::map<uint8_t, Live> live;
    std::deque<double> scope;
    clock::Timeline tl;
    std::vector<net::Device> devices;
    net::Counters counters{};
    uint32_t masterTrackId = 0;
    uint8_t masterTrackSlot = 0;
    size_t writtenEvents = 0, writtenRows = 0;
    std::string claimState;
    int claimNumber = 0;
};

class Core::HostImpl : public FeatureHost {
public:
    explicit HostImpl(Core& c) : c_(c) {}
    int64_t wallNowMs() const override;
    MetaStore& meta() override { return c_.meta_; }
    ControlRegistry& controls() override { return c_.controls_; }
    out::OutputManager& outputs() override { return c_.outputs_; }
    Json settings(const std::string& featureId) override;
    bool saveSettings(const std::string& featureId, const Json& value) override;
    const clock::ClockEngine& engine() const override { return c_.engine_; }
    void onSessionCleared(std::function<void()> cb) override;
    bool enterReplay(ReplayHooks* hooks) override { return c_.enterReplay(hooks); }
    void leaveReplay() override { c_.leaveReplay(); }
    bool replaying() const override;
    bool feed(uint16_t port, const uint8_t* data, size_t len, int64_t recvNs, uint32_t srcIp, int64_t wallMs) override;
    void resetEngine(bool clearSession) override;
    void setOutputsEnabled(bool on) override;
    bool hasCoreConfig() const override { return true; }
    Json config() override;
    bool applyConfig(const Json& patch, std::string& error) override;
    std::string configDir() const override;
    std::string dataDir() const override { return c_.dataDir_; }

private:
    Core& c_;
};

} // namespace shunt::app
