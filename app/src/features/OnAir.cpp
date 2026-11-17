// FS-01 on-air routing and clock mute: OnAirAggregator (pure logic) and the "onair" feature.
// Spec: docs/specs/FS-01-onair-routing-and-clock-mute.md. Original Shunt code.
#include "OnAir.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace shunt::app::onair {

// ---- OnAirAggregator ---------------------------------------------------------------------------

void OnAirAggregator::configure(const OnAirConfig& c) {
    cfg_ = c;
    cfg_.holdNs = std::max<int64_t>(0, cfg_.holdNs);
    for (auto& ch : cfg_.channelMap) ch = uint8_t(std::clamp<int>(ch, 1, kDecks));
}

void OnAirAggregator::reset() {
    const OnAirConfig c = cfg_;
    *this = OnAirAggregator();
    cfg_ = c;
}

bool OnAirAggregator::anyPresent() const {
    for (int d = 1; d <= kDecks; ++d) if (decks_[d].present) return true;
    return false;
}

bool OnAirAggregator::deckOn(uint8_t deck, int64_t nowNs) const {
    if (!deckPresent(deck)) return false;
    if (mixerFresh(nowNs)) {
        const int idx = cfg_.channelMap[deck - 1] - 1;
        return idx < mixerCount_ && mixer_[idx] != 0;
    }
    return decks_[deck].bit;
}

const char* OnAirAggregator::source(int64_t nowNs) const {
    if (mixerFresh(nowNs)) return "mixer";
    return anyPresent() ? "players" : "none";
}

void OnAirAggregator::onPlayerBit(uint8_t deck, bool on, int64_t nowNs) {
    if (deck < 1 || deck > kDecks) return;
    decks_[deck].present = true;
    decks_[deck].bit = on;
    if (on) dataSeen_ = true;
    evaluate(nowNs);
}

void OnAirAggregator::onMixerChannels(const uint8_t ch[6], int count, int64_t nowNs) {
    mixerCount_ = std::clamp(count, 0, kDecks);
    for (int i = 0; i < kDecks; ++i) mixer_[i] = i < mixerCount_ ? ch[i] : 0;
    lastMixerNs_ = nowNs;
    mixerSeen_ = dataSeen_ = true;
    evaluate(nowNs);
}

void OnAirAggregator::onDeckLeft(uint8_t deck, int64_t nowNs) {
    if (deck < 1 || deck > kDecks) return;
    decks_[deck].present = false;          // keep `on` so evaluate() reports the edge
    decks_[deck].bit = false;
    evaluate(nowNs);
}

void OnAirAggregator::evaluate(int64_t nowNs) {
    bool anyNow = false;
    for (int d = 1; d <= kDecks; ++d) {
        Deck& k = decks_[d];
        const bool on = deckOn(uint8_t(d), nowNs);
        if (on != k.on) { k.on = on; pending_.push_back({Edge::DeckChanged, on, uint8_t(d)}); }
        anyNow = anyNow || on;
    }
    if (anyNow != any_) {
        any_ = anyNow;
        lastChangeNs_ = nowNs;
        pending_.push_back({Edge::AnyChanged, anyNow, 0});
    }
    // The hold runs only while every mute condition holds; it restarts when one comes back.
    const bool canMute = cfg_.muteEnabled && dataSeen_ && anyPresent() && !any_;
    if (!canMute) holdArmed_ = false;
    else if (!holdArmed_) { holdArmed_ = true; holdFromNs_ = nowNs; }
    if (muted_ && (any_ || !cfg_.muteEnabled)) {   // unmute at once (deck back on air, or policy off)
        muted_ = false;
        lastChangeNs_ = nowNs;
        pending_.push_back({Edge::MuteChanged, false, 0});
    } else if (!muted_ && holdArmed_ && nowNs - holdFromNs_ >= cfg_.holdNs) {
        muted_ = true;
        lastChangeNs_ = nowNs;
        pending_.push_back({Edge::MuteChanged, true, 0});
    }
}

std::vector<Edge> OnAirAggregator::tick(int64_t nowNs) {
    evaluate(nowNs);
    if (pending_.empty()) return {};
    return std::exchange(pending_, {});
}

// ---- settings ----------------------------------------------------------------------------------

OnAirSettings OnAirSettings::fromJson(const Json& j) {
    OnAirSettings s;
    if (!j.isObject()) return s;
    s.enabled = j.get("enabled").asBool(false);
    const Json& hold = j.get("holdMs");
    if (hold.type() == Json::Number && std::isfinite(hold.asNumber()))
        s.holdMs = int(std::clamp(hold.asNumber(), 500.0, 30000.0));
    s.muteStop = j.get("midiMute").asString("off") == "stop";
    s.unmuteWith = j.get("unmuteWith").asString("start") == "continue" ? out::UnmuteMode::Continue : out::UnmuteMode::Start;
    const Json& map = j.get("channelMap");
    if (map.isArray() && map.size() == kDecks) {
        for (int i = 0; i < kDecks; ++i) {
            const Json& e = map.items()[size_t(i)];
            const double v = e.asNumber(double(i + 1));
            s.channelMap[size_t(i)] = uint8_t(std::isfinite(v) ? std::clamp(int(std::lround(std::clamp(v, -1e6, 1e6))), 1, kDecks) : i + 1);
        }
    }
    s.oscPerDeck = j.get("oscPerDeck").asBool(true);
    auto cue = [&](const char* k) {
        const Json& c = j.get(k);
        return c.type() == Json::Number && std::isfinite(c.asNumber()) ? int(std::clamp(c.asNumber(), 0.0, 9999.0)) : 0;
    };
    s.qlabOnAirCue = cue("qlabOnAirCue");
    s.qlabMuteCue = cue("qlabMuteCue");
    return s;
}

Json OnAirSettings::toJson() const {
    Json j = Json::object();
    j["enabled"] = enabled;
    j["holdMs"] = holdMs;
    j["midiMute"] = muteStop ? "stop" : "off";
    j["unmuteWith"] = unmuteWith == out::UnmuteMode::Continue ? "continue" : "start";
    Json m = Json::array();
    for (uint8_t c : channelMap) m.push(int(c));
    j["channelMap"] = m;
    j["oscPerDeck"] = oscPerDeck;
    j["qlabOnAirCue"] = qlabOnAirCue;
    j["qlabMuteCue"] = qlabMuteCue;
    return j;
}

OnAirConfig OnAirSettings::config() const {
    OnAirConfig c;
    c.holdNs = int64_t(holdMs) * 1'000'000;
    c.channelMap = channelMap;
    c.muteEnabled = muteStop;
    return c;
}

// ---- feature -----------------------------------------------------------------------------------

void OnAirFeature::init(FeatureHost& host, const Json& settings) {
    host_ = &host;
    applySettings(settings);
}

void OnAirFeature::pushOutputs(const OnAirSettings& s, bool muted) {
    if (!host_) return;
    out::OutputManager& o = host_->outputs();
    o.setUnmuteMode(s.unmuteWith);
    o.setMute(s.enabled && s.muteStop && muted);
    o.setLegacyOnAirOsc(!s.enabled);       // enabled: per-deck /shunt/onair comes from here (oscPerDeck)
}

void OnAirFeature::applySettings(const Json& settings) {
    const OnAirSettings s = OnAirSettings::fromJson(settings);
    std::lock_guard<std::mutex> lk(mu_);
    const bool wasEnabled = s_.enabled;
    s_ = s;
    agg_.configure(s.config());
    if (!s.enabled || !wasEnabled) { agg_.reset(); rekordbox_.fill(false); }   // enabling starts clean
    pushOutputs(s, agg_.muted());          // atomic stores only, so done under mu_ (ordered with onTick)
}

void OnAirFeature::onEvent(const net::Event& ev, int64_t) {
    std::lock_guard<std::mutex> lk(mu_);
    switch (ev.type) {
    case net::EventType::PlayerStatus:
        if (!rekordbox_[ev.device]) agg_.onPlayerBit(ev.device, ev.status.onAir, ev.recvTimeNs);   // rekordbox peers ignored
        break;
    case net::EventType::OnAir:
        agg_.onMixerChannels(ev.onAir.channels, ev.onAir.channelCount, ev.recvTimeNs);
        break;
    case net::EventType::Device:
        if (ev.deviceEvent.kind == net::DeviceEvent::Left) { rekordbox_[ev.device] = false; agg_.onDeckLeft(ev.device, ev.recvTimeNs); }
        else if (ev.deviceEvent.deviceKind == net::DeviceKind::Rekordbox) { rekordbox_[ev.device] = true; agg_.onDeckLeft(ev.device, ev.recvTimeNs); }
        break;
    default: break;
    }
}

void OnAirFeature::onTick(int64_t nowNs, int64_t) {
    if (!host_) return;
    out::OutputManager& o = host_->outputs();
    std::vector<Edge> edges;
    OnAirSettings s;
    {
        std::lock_guard<std::mutex> lk(mu_);
        lastNowNs_ = nowNs;
        edges = agg_.tick(nowNs);
        o.setMute(s_.enabled && agg_.muted());   // atomic; muted is only ever true with midiMute stop
        if (edges.empty()) return;
        s = s_;
    }
    if (!o.oscEnabled()) return;           // OutputManager lock taken after mu_ is released
    const out::OscProfile p = o.oscProfile();
    const bool shunt = p == out::OscProfile::Generic || p == out::OscProfile::MagicQ;
    for (const Edge& e : edges) {
        const int v = e.value ? 1 : 0;
        switch (e.kind) {
        case Edge::DeckChanged:
            if (shunt && s.oscPerDeck) o.sendOsc("/shunt/onair", {out::OscArg::Int(e.deck), out::OscArg::Int(v)});
            break;
        case Edge::AnyChanged:
            if (shunt) o.sendOsc("/shunt/onair/any", {out::OscArg::Int(v)});
            else if (p == out::OscProfile::QLab && e.value && s.qlabOnAirCue > 0) o.sendOsc("/cue/" + std::to_string(s.qlabOnAirCue) + "/start");
            break;
        case Edge::MuteChanged:
            if (shunt) o.sendOsc("/shunt/mute", {out::OscArg::Int(v)});
            else if (p == out::OscProfile::QLab && e.value && s.qlabMuteCue > 0) o.sendOsc("/cue/" + std::to_string(s.qlabMuteCue) + "/start");
            break;
        }
    }
}

Json OnAirFeature::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    const int64_t now = lastNowNs_;
    Json j = Json::object();
    j["any"] = agg_.any();
    j["muted"] = agg_.muted();
    j["dataSeen"] = agg_.dataSeen();
    j["source"] = s_.enabled ? agg_.source(now) : "none";
    Json decks = Json::array();
    for (int d = 1; d <= kDecks; ++d) {
        if (!agg_.deckPresent(uint8_t(d))) continue;
        Json o = Json::object();
        o["deck"] = d;
        o["on"] = agg_.deckOn(uint8_t(d), now);
        decks.push(o);
    }
    j["decks"] = decks;
    j["sinceMs"] = agg_.lastChangeNs() > 0 && now >= agg_.lastChangeNs() ? double((now - agg_.lastChangeNs()) / 1'000'000) : 0.0;
    return j;
}

std::optional<HttpResponse> OnAirFeature::handle(const HttpRequest& req, const std::string& subpath) {
    if (subpath != "settings") return std::nullopt;
    HttpResponse r;
    if (req.method != "GET" && req.method != "HEAD") {
        r.status = 405;
        r.body = "{\"ok\":false,\"error\":\"GET only\"}";
        return r;
    }
    const Json raw = host_ ? host_->settings("onair") : Json::object();   // no own lock held (rule 3)
    r.body = OnAirSettings::fromJson(raw).toJson().dump();
    return r;
}

void OnAirFeature::shutdown() {
    if (!host_) return;
    host_->outputs().setMute(false);
    host_->outputs().setLegacyOnAirOsc(true);
}

} // namespace shunt::app::onair

SHUNT_REGISTER_FEATURE(shunt::app::onair::OnAirFeature);
