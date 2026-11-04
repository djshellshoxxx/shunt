// FS-02 key clash alarm (docs/specs/FS-02-key-clash-alarm.md). KeyClashLogic is the pure part (synthetic
// time, no threads, no I/O; the key lookup is a callback): per-deck state from player status events,
// on-air state through onair::OnAirAggregator (a private instance, same rules as FS-01), reference /
// cued pair selection, key level and the debounced alarm. KeyClashFeature wires it to FS-00
// (features.keyclash, status.features.keyclash, GET /api/f/keyclash/settings, OSC).
// Original Shunt code.
#pragma once
#include "KeyMath.h"
#include "OnAir.h"
#include "shunt/app/Feature.h"
#include <array>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace shunt::app::keyclash {

constexpr int kDecks = onair::kDecks;
constexpr int64_t kEvalPeriodNs = 200'000'000;     // 5 Hz

struct KeyClashSettings {
    bool enabled = false;
    bool strict = false;                  // energy level also raises the alarm
    bool assumeKeyLock = true;            // true: pitch is ignored (R6: key lock flag is not parsed)
    int debounceMs = 1500;                // clamped 0..30000
    bool oscEnabled = true;
    int qlabKeyClashCue = 0;              // 0 = off, clamped 0..9999
    bool experimentalStatusKey = false;   // UNVERIFIED status-byte decode (K0); XML always wins

    static KeyClashSettings fromJson(const Json& j);   // validates and clamps; never throws
    Json toJson() const;
};

enum class Source : uint8_t { None, Xml, Status };
const char* sourceName(Source s);

struct DeckKey {
    int deck = 0;
    std::optional<Camelot> key;           // after the optional pitch shift
    Source source = Source::None;
    std::string text;                     // toString(*key) or ""
};

struct KeySnapshot {
    bool pair = false;                    // reference and cued decks both exist
    Level level = Level::Unknown;         // meaningful only when pair
    bool alarm = false;
    DeckKey reference, cued;
    std::array<Source, kDecks + 1> sources{};   // per loaded deck, index 1..6
    std::array<bool, kDecks + 1> loaded{};
};

struct Notice {                           // one OSC message
    bool alarm;
    bool alarmEdge;                       // the alarm bit changed
    std::string ref, cued;
};

class KeyClashLogic {
public:
    using Lookup = std::function<std::optional<std::string>(uint32_t trackId)>;

    void configure(const KeyClashSettings& s, const onair::OnAirConfig& oa);
    void reset();
    void onEvent(const net::Event& ev);                                  // allocation-free except on on-air edges
    // Evaluates at most every kEvalPeriodNs (returns {} in between). Returns the notices since last call.
    std::vector<Notice> tick(int64_t nowNs, const Lookup& lookup);
    const KeySnapshot& snapshot() const { return snap_; }
    const onair::OnAirConfig& onAirConfig() const { return oaCfg_; }

private:
    struct Deck {
        bool present = false, playing = false, master = false, hasStatusKey = false;
        uint32_t trackId = 0;
        uint8_t trackType = 0;
        double pitch = 1.0;
        uint8_t key[3] = {0, 0, 0};
        bool loaded() const { return present && trackId != 0 && trackType != 0; }
    };
    DeckKey keyOf(int d, const Lookup& lookup) const;
    void evaluate(int64_t nowNs, const Lookup& lookup, std::vector<Notice>& out);

    KeyClashSettings s_;
    onair::OnAirConfig oaCfg_;
    onair::OnAirAggregator agg_;
    std::array<Deck, kDecks + 1> decks_{};
    std::array<bool, 256> rekordbox_{};
    KeySnapshot snap_;
    int64_t lastEvalNs_ = 0;
    bool evaluated_ = false;
    bool candidate_ = false;              // raw alarm condition
    int64_t candidateSinceNs_ = 0;
    bool emittedAlarm_ = false;
    std::string emittedRef_, emittedCued_;
};

class KeyClashFeature : public IFeature {
public:
    std::string id() const override { return "keyclash"; }
    void init(FeatureHost& host, const Json& settings) override;
    void applySettings(const Json& settings) override;
    void onEvent(const net::Event& ev, int64_t wallMs) override;
    void onTick(int64_t nowNs, int64_t wallMs) override;
    Json status() const override;
    std::optional<HttpResponse> handle(const HttpRequest& req, const std::string& subpath) override;

private:
    onair::OnAirConfig onAirConfig() const;           // FS-01 channel map, read from its settings

    FeatureHost* host_ = nullptr;
    mutable std::mutex mu_;
    KeyClashSettings s_;
    KeyClashLogic logic_;
    int64_t lastOaPollNs_ = 0;
};

} // namespace shunt::app::keyclash
