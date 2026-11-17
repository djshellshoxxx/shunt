// FS-06 downbeat tap, fine nudge and confidence breakdown (docs/specs/FS-06-downbeat-tap-and-nudge.md).
// Pure helpers (tapGeometry, offsetForRaw, NudgeSettings) take time as an argument; NudgeFeature wires
// them to the FS-00 IFeature interface: cached timeline snapshot (engine() is only read inside
// onEvent/onTick/status), HTTP routes under /api/f/nudge/, and the bindable actions nudge.* and
// downbeat.* registered through FeatureHost::controls().
// Original Shunt code.
#pragma once
#include "shunt/app/Feature.h"
#include "shunt/clock/ClockEngine.h"
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

namespace shunt::app::nudge {

constexpr int64_t kRateLimitNs = 300'000'000;      // taps closer than this to the previous one are ignored
constexpr int64_t kUndoExpiryNs = 60'000'000'000;  // single level undo is dropped after this
constexpr int64_t kStaleNs = 1'000'000'000;        // snapshot older than this: no timing data
constexpr double kMaxBeatDistance = 0.35;
constexpr double kMinConfidence = 0.5;
constexpr size_t kLogSize = 20;

struct NudgeSettings {
    bool enabled = true;
    int tapLatencyMs = 60;                          // 0..250
    bool wakeLock = true, haptics = true;
    static NudgeSettings fromJson(const Json& j);   // validates and clamps; never throws
    Json toJson() const;
};

struct TapGeometry {
    int64_t k = 0;            // nearest beat index
    double distanceBeats = 0; // |t - timeOfBeat(k)| / period
    int raw = 1;              // packet beat-in-bar (1..4) of beat k
    int newOffset = 0;        // user bar offset that makes beat k a downbeat (0..3)
};
// userOffset: the bar offset that was in effect when `tl` was published.
TapGeometry tapGeometry(const clock::Timeline& tl, int userOffset, int64_t tapNs);
int offsetForRaw(int raw);                          // (1 - raw) mod 4, non-negative
// Why a tap would be refused (empty: allowed). ageNs = time since the timeline snapshot (< 0: skip the staleness check).
std::string tapBlockReason(const clock::Timeline& tl, clock::State state, bool beatOnly, bool valid, int64_t ageNs);

class NudgeFeature : public IFeature {
public:
    NudgeFeature();
    std::string id() const override { return "nudge"; }
    bool defaultEnabled() const override { return true; }
    void init(FeatureHost& host, const Json& settings) override;
    void applySettings(const Json& settings) override;
    void onEvent(const net::Event& ev, int64_t wallMs) override;
    void onTick(int64_t nowNs, int64_t wallMs) override;
    Json status() const override;
    std::optional<HttpResponse> handle(const HttpRequest& req, const std::string& subpath) override;
    void shutdown() override;

    // Monotonic clock used by handle() and the actions (same clock as engine event times).
    // Replaceable for tests; set before use.
    void setClock(std::function<int64_t()> c) { clock_ = std::move(c); }

private:
    struct Snap {
        bool valid = false;
        int64_t atNs = 0;
        clock::Timeline tl;
        clock::State state = clock::State::Idle;
        int offsetAtPublish = 0;     // bar offset in effect when tl was published
        bool offsetKnown = false;
        int cfgOffset = 0;           // current setting
        bool beatOnly = false;
        double latencyMs = 1.0;
    };
    struct Undo { bool valid = false; int previous = 0; int64_t atNs = 0; uint64_t trackKey = 0; };
    struct LogEntry { int64_t wallMs; double from, to; };
    struct Result { int status = 200; Json body; };

    void refreshLocked(int64_t nowNs);                       // needs engine(): Core-locked callers only
    Result doTap();
    Result doUndo();
    Result doStep(double ms, bool exactRequested);
    Result doReset();
    Result applyLatency(double target, double cur);
    static Result fail(int status, const std::string& msg);
    void registerActions();
    Json settingsJson() const;

    FeatureHost* host_ = nullptr;
    std::function<int64_t()> clock_;
    mutable std::mutex mu_;
    NudgeSettings s_;
    Snap snap_;
    Undo undo_;
    uint64_t trackKey_ = 0;
    bool hasTap_ = false;
    int64_t lastTapNs_ = 0, lastNowNs_ = 0;
    double latSeen_ = 0, baseline_ = 1.0, pending_ = 0;
    bool latKnown_ = false;
    std::deque<LogEntry> log_;
    std::vector<std::string> actionIds_;
};

} // namespace shunt::app::nudge
