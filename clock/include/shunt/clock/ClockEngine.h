// Clock engine (ES-03): turns the master deck's beat and status events into one
// smooth, predictable beat timeline. No sockets, no threads.
#pragma once
#include "shunt/net/Events.h"
#include <cstdint>
#include <array>

namespace shunt::clock {

enum class Profile : uint8_t { Stage, Rehearsal };
enum class State : uint8_t { Idle, Locking, Locked, Coasting, Paused, MixerMaster };

struct Config {
    int64_t latencyOffsetNs = 1'000'000;   // -20 ms .. +50 ms
    int userBarOffset = 0;                 // 0..3
    Profile profile = Profile::Stage;
    bool beatOnlyMode = false;
};

struct Timeline {
    double bpm = 0;
    int64_t beatOriginNs = 0;
    int64_t beatIndex = 0;
    int beatInBar = 1;
    bool barKnown = false;
    bool playing = false;
    float confidence = 0;
    uint32_t sequence = 0;
    uint32_t resetSequence = 0;
    uint8_t masterDevice = 0;

    int64_t timeOfBeat(int64_t k) const {
        return bpm > 0 ? beatOriginNs + int64_t(double(k - beatIndex) * 60e9 / bpm) : beatOriginNs;
    }
    double periodNs() const { return bpm > 0 ? 60e9 / bpm : 0; }
};

struct PlayerTrack {
    bool present = false;
    net::Model model = net::Model::Unknown;
    bool isMixer = false;
    int64_t lastBeatRecvNs = 0;
    uint8_t lastBeatInBar = 0;
    double reportedBpm = 0;            // bpm100 * pitch1 from status, fallback beat packet
    bool statusSeen = false;
    uint32_t beatNumber = 0xffffffff;
    int64_t beatNumberNs = 0;
    bool playing = true;               // assumed true until a status says otherwise
    bool onAir = false;
    uint8_t trackType = 1;
    double lastPitch1 = 1.0;
    // filter
    int64_t phaseNs = 0;
    double periodNs = 0;
    bool hasPhase = false;
    int boostBeatsLeft = 0;
    int64_t lastAcceptedNs = 0;
    int64_t beatIndex = 0;
    std::array<double, 16> residualHistory{};
    int residualCount = 0, residualPos = 0;
    int outlierRun = 0, outlierSign = 0;
    std::array<uint8_t, 8> hits{};
    int hitPos = 0, hitCount = 0;
    int64_t beatsSinceReset = 0;
    bool pendingJumpCheck = false;

    double mad() const;
    double rmsResidual() const;
    double hitRate() const;
};

enum class BeatOutcome : uint8_t { First, Accepted, Coast, Reset, Duplicate };

class ClockEngine {
public:
    explicit ClockEngine(const Config& cfg = Config{});

    void onEvent(const net::Event& ev);
    void tick(int64_t nowNs);

    const Timeline& timeline() const { return tl_; }
    State state() const;
    uint8_t master() const { return master_; }
    const PlayerTrack& track(uint8_t device) const { return tracks_[device]; }
    const Config& config() const { return cfg_; }
    void setConfig(const Config& c) { cfg_ = c; }
    void setUserBarOffset(int offset) { cfg_.userBarOffset = offset & 3; }
    BeatOutcome lastOutcome() const { return lastOutcome_; }

private:
    BeatOutcome processBeat(PlayerTrack& tr, int64_t t, bool isMaster);
    void resetTrack(PlayerTrack& tr, int64_t t);
    void onMasterChanged(uint8_t from, uint8_t to, int64_t now);
    void publish(const PlayerTrack& tr, uint8_t packetBeatInBar);
    void updateConfidence(const PlayerTrack& tr);
    double alpha(const PlayerTrack& tr) const;
    double beta() const;

    Config cfg_;
    std::array<PlayerTrack, 256> tracks_{};
    Timeline tl_;
    State state_ = State::Idle;
    uint8_t master_ = 0;
    int lockBeats_ = 0;
    bool resetOnLock_ = false;
    int64_t coastStartNs_ = 0;
    int64_t lastTickNs_ = 0;
    BeatOutcome lastOutcome_ = BeatOutcome::First;
};

// Publishing policy helper (ES-03 section 7), consumed by the outputs.
class PublishPolicy {
public:
    static constexpr double kTempoDelta = 0.02;
    static constexpr int64_t kTempoIntervalNs = 1'000'000'000LL;
    static constexpr double kPhaseErrorBeats = 1.0 / 60.0;

    enum class PhaseAction : uint8_t { None, Soft, Hard };

    // Returns true when the outputs should apply tl.bpm now (and records the publish).
    bool shouldPublishTempo(const Timeline& tl, int64_t nowNs);
    // Call once per beat with the output's phase error in beats.
    PhaseAction phaseDecision(double phaseErrorBeats, const Timeline& tl);

    double lastPublishedBpm() const { return lastBpm_; }

private:
    double lastBpm_ = -1;
    int64_t lastPublishNs_ = -(1LL << 60);
    uint32_t lastResetSeq_ = 0;
    uint32_t phaseResetSeq_ = 0;
    int consecutiveErrors_ = 0;
    bool first_ = true;
};

} // namespace shunt::clock
