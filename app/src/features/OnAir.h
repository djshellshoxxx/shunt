// FS-01 on-air routing and clock mute (docs/specs/FS-01-onair-routing-and-clock-mute.md).
// OnAirAggregator is the pure logic (synthetic time, no threads, no I/O): per-deck on-air state
// from player F-byte bits and mixer 0x03 packets (mixer authoritative while fresh, 2 s), the
// aggregate `any`, and the hold timer that mutes the MIDI clock. OnAirFeature wires it to the
// FS-00 IFeature interface (settings features.onair, status.features.onair, OSC edges, mute flag).
// Original Shunt code.
#pragma once
#include "shunt/app/Feature.h"
#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

namespace shunt::app::onair {

constexpr int kDecks = 6;
constexpr int64_t kMixerStaleNs = 2'000'000'000;   // mixer authoritative while its last 0x03 is this fresh

struct OnAirConfig {
    int64_t holdNs = 4'000'000'000;
    std::array<uint8_t, kDecks> channelMap{{1, 2, 3, 4, 5, 6}};   // deck d -> mixer channel (1..6)
    bool muteEnabled = true;                                      // midiMute == stop
};

struct Edge {
    enum Kind : uint8_t { AnyChanged, MuteChanged, DeckChanged } kind;
    bool value;
    uint8_t deck;                // DeckChanged only, 1..6
};

class OnAirAggregator {
public:
    void configure(const OnAirConfig& c);
    const OnAirConfig& config() const { return cfg_; }
    void reset();                                        // forget everything (feature disabled)

    void onPlayerBit(uint8_t deck, bool on, int64_t nowNs);              // deck 1..6, others ignored
    void onMixerChannels(const uint8_t ch[6], int count, int64_t nowNs);  // count clamped 0..6
    void onDeckLeft(uint8_t deck, int64_t nowNs);
    // Re-evaluates (mixer staleness, hold timer) and returns the edges since the last call,
    // in order. Allocation-free when there are no edges.
    std::vector<Edge> tick(int64_t nowNs);

    bool deckOn(uint8_t deck, int64_t nowNs) const;
    bool deckPresent(uint8_t deck) const { return deck >= 1 && deck <= kDecks && decks_[deck].present; }
    bool any() const { return any_; }
    bool muted() const { return muted_; }
    bool dataSeen() const { return dataSeen_; }
    bool mixerFresh(int64_t nowNs) const { return mixerSeen_ && nowNs - lastMixerNs_ <= kMixerStaleNs; }
    const char* source(int64_t nowNs) const;             // "mixer", "players" or "none"
    int64_t lastChangeNs() const { return lastChangeNs_; }   // last any/mute edge (0 = never)

private:
    struct Deck { bool present = false, bit = false, on = false; };
    void evaluate(int64_t nowNs);
    bool anyPresent() const;

    OnAirConfig cfg_;
    std::array<Deck, kDecks + 1> decks_{};               // index 1..6
    uint8_t mixer_[kDecks] = {0, 0, 0, 0, 0, 0};
    int mixerCount_ = 0;
    int64_t lastMixerNs_ = 0;
    bool mixerSeen_ = false, dataSeen_ = false, any_ = false, muted_ = false;
    bool holdArmed_ = false;
    int64_t holdFromNs_ = 0, lastChangeNs_ = 0;
    std::vector<Edge> pending_;
};

struct OnAirSettings {
    bool enabled = false;
    int holdMs = 4000;                                   // clamped 500..30000
    bool muteStop = false;                               // midiMute: "off" | "stop"
    out::UnmuteMode unmuteWith = out::UnmuteMode::Start; // unmuteWith: "start" | "continue"
    std::array<uint8_t, kDecks> channelMap{{1, 2, 3, 4, 5, 6}};
    bool oscPerDeck = true;
    int qlabOnAirCue = 0, qlabMuteCue = 0;               // 0 = off, clamped 0..9999

    static OnAirSettings fromJson(const Json& j);        // validates and clamps; never throws
    Json toJson() const;
    OnAirConfig config() const;
};

// The IFeature. Locking: own mutex mu_, taken after the Core lock (onEvent/onTick/status) or
// alone (applySettings/handle); OutputManager calls happen after mu_ is released.
class OnAirFeature : public IFeature {
public:
    std::string id() const override { return "onair"; }
    void init(FeatureHost& host, const Json& settings) override;
    void applySettings(const Json& settings) override;
    void onEvent(const net::Event& ev, int64_t wallMs) override;
    void onTick(int64_t nowNs, int64_t wallMs) override;
    Json status() const override;
    std::optional<HttpResponse> handle(const HttpRequest& req, const std::string& subpath) override;
    void shutdown() override;

private:
    void pushOutputs(const OnAirSettings& s, bool muted);

    FeatureHost* host_ = nullptr;
    mutable std::mutex mu_;
    OnAirSettings s_;
    OnAirAggregator agg_;
    std::array<bool, 256> rekordbox_{};                  // device numbers announced as rekordbox
    int64_t lastNowNs_ = 0;
};

} // namespace shunt::app::onair
