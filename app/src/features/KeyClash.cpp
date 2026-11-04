// FS-02 key clash alarm: KeyClashLogic (pure) and the "keyclash" feature.
// Spec: docs/specs/FS-02-key-clash-alarm.md. Original Shunt code.
#include "KeyClash.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace shunt::app::keyclash {

const char* sourceName(Source s) {
    switch (s) {
    case Source::Xml: return "xml";
    case Source::Status: return "status";
    case Source::None: break;
    }
    return "none";
}

// ---- settings ----------------------------------------------------------------------------------

KeyClashSettings KeyClashSettings::fromJson(const Json& j) {
    KeyClashSettings s;
    if (!j.isObject()) return s;
    s.enabled = j.get("enabled").asBool(false);
    s.strict = j.get("strict").asBool(false);
    s.assumeKeyLock = j.get("assumeKeyLock").asBool(true);
    s.oscEnabled = j.get("oscEnabled").asBool(true);
    s.experimentalStatusKey = j.get("experimentalStatusKey").asBool(false);
    const Json& d = j.get("debounceMs");
    if (d.type() == Json::Number && std::isfinite(d.asNumber())) s.debounceMs = int(std::clamp(d.asNumber(), 0.0, 30000.0));
    const Json& c = j.get("qlabKeyClashCue");
    if (c.type() == Json::Number && std::isfinite(c.asNumber())) s.qlabKeyClashCue = int(std::clamp(c.asNumber(), 0.0, 9999.0));
    return s;
}

Json KeyClashSettings::toJson() const {
    Json j = Json::object();
    j["enabled"] = enabled;
    j["strict"] = strict;
    j["assumeKeyLock"] = assumeKeyLock;
    j["debounceMs"] = debounceMs;
    j["oscEnabled"] = oscEnabled;
    j["qlabKeyClashCue"] = qlabKeyClashCue;
    j["experimentalStatusKey"] = experimentalStatusKey;
    return j;
}

// ---- logic -------------------------------------------------------------------------------------

void KeyClashLogic::configure(const KeyClashSettings& s, const onair::OnAirConfig& oa) {
    s_ = s;
    oaCfg_ = oa;
    agg_.configure(oa);
}

void KeyClashLogic::reset() {
    agg_.reset();
    decks_ = {};
    rekordbox_.fill(false);
    snap_ = KeySnapshot{};
    evaluated_ = false;
    lastEvalNs_ = 0;
    candidate_ = false;
    emittedAlarm_ = false;
    emittedRef_.clear();
    emittedCued_.clear();
}

void KeyClashLogic::onEvent(const net::Event& ev) {
    switch (ev.type) {
    case net::EventType::PlayerStatus: {
        if (ev.device < 1 || ev.device > kDecks || rekordbox_[ev.device]) break;
        Deck& d = decks_[ev.device];
        const net::PlayerStatusEvent& p = ev.status;
        d.present = true;
        d.playing = p.playing;
        d.master = p.master;
        d.trackId = p.rekordboxId;
        d.trackType = p.trackType;
        d.pitch = p.pitch1;
        d.hasStatusKey = p.hasKey;
        if (p.hasKey) { d.key[0] = p.key[0]; d.key[1] = p.key[1]; d.key[2] = p.key[2]; }
        agg_.onPlayerBit(ev.device, p.onAir, ev.recvTimeNs);
        break;
    }
    case net::EventType::OnAir:
        agg_.onMixerChannels(ev.onAir.channels, ev.onAir.channelCount, ev.recvTimeNs);
        break;
    case net::EventType::Device:
        if (ev.device < 1 || ev.device > kDecks) break;
        if (ev.deviceEvent.kind == net::DeviceEvent::Left) {
            rekordbox_[ev.device] = false;
            decks_[ev.device] = Deck{};
            agg_.onDeckLeft(ev.device, ev.recvTimeNs);
        } else if (ev.deviceEvent.deviceKind == net::DeviceKind::Rekordbox) {
            rekordbox_[ev.device] = true;
            decks_[ev.device] = Deck{};
            agg_.onDeckLeft(ev.device, ev.recvTimeNs);
        }
        break;
    default: break;
    }
}

DeckKey KeyClashLogic::keyOf(int d, const Lookup& lookup) const {
    DeckKey k;
    k.deck = d;
    const Deck& dk = decks_[size_t(d)];
    std::optional<Camelot> c;
    if (dk.trackId != 0 && lookup) {
        if (auto txt = lookup(dk.trackId)) { c = parseKey(*txt); if (c) k.source = Source::Xml; }
    }
    if (!c && s_.experimentalStatusKey && dk.hasStatusKey) {
        c = experimentalDecodeStatusKey(dk.key);
        if (c) k.source = Source::Status;
    }
    if (c && !s_.assumeKeyLock) c = shift(*c, semitonesForPitch(dk.pitch));
    k.key = c;
    if (c) k.text = toString(*c);
    return k;
}

void KeyClashLogic::evaluate(int64_t nowNs, const Lookup& lookup, std::vector<Notice>& out) {
    KeySnapshot n;
    int ref = 0;
    for (int d = 1; d <= kDecks; ++d) {                      // master if on air, else lowest on-air deck
        if (!agg_.deckOn(uint8_t(d), nowNs)) continue;
        if (decks_[size_t(d)].master) { ref = d; break; }
        if (!ref) ref = d;
    }
    int cued = 0;
    for (int d = 1; d <= kDecks; ++d) {                      // loaded, not on air: playing first, else lowest
        const Deck& dk = decks_[size_t(d)];
        if (d == ref || !dk.loaded() || agg_.deckOn(uint8_t(d), nowNs)) continue;
        if (!cued || (dk.playing && !decks_[size_t(cued)].playing)) cued = d;
    }
    for (int d = 1; d <= kDecks; ++d) n.loaded[size_t(d)] = decks_[size_t(d)].loaded();
    if (ref && cued) {
        n.pair = true;
        n.reference = keyOf(ref, lookup);
        n.cued = keyOf(cued, lookup);
        n.level = level(n.reference.key, n.cued.key);
        n.sources[size_t(ref)] = n.reference.source;
        n.sources[size_t(cued)] = n.cued.source;
    }
    for (int d = 1; d <= kDecks; ++d)                         // sources of the other loaded decks (status display)
        if (n.loaded[size_t(d)] && d != ref && d != cued) n.sources[size_t(d)] = keyOf(d, lookup).source;

    const bool cand = n.pair && (n.level == Level::Clash || (s_.strict && n.level == Level::Energy));
    if (!evaluated_ || cand != candidate_) { candidate_ = cand; candidateSinceNs_ = nowNs; }
    evaluated_ = true;
    n.alarm = snap_.alarm;
    const int64_t debounceNs = int64_t(s_.debounceMs) * 1'000'000;
    if (candidate_ != n.alarm && nowNs - candidateSinceNs_ >= debounceNs) n.alarm = candidate_;
    snap_ = std::move(n);

    const bool edge = snap_.alarm != emittedAlarm_;
    if (edge || snap_.reference.text != emittedRef_ || snap_.cued.text != emittedCued_) {
        emittedAlarm_ = snap_.alarm;
        emittedRef_ = snap_.reference.text;
        emittedCued_ = snap_.cued.text;
        out.push_back(Notice{snap_.alarm, edge, emittedRef_, emittedCued_});
    }
}

std::vector<Notice> KeyClashLogic::tick(int64_t nowNs, const Lookup& lookup) {
    std::vector<Notice> out;
    agg_.tick(nowNs);                                         // drain pending edges, apply mixer staleness
    if (lastEvalNs_ != 0 && nowNs - lastEvalNs_ < kEvalPeriodNs) return out;
    lastEvalNs_ = nowNs ? nowNs : 1;
    evaluate(nowNs, lookup, out);
    return out;
}

// ---- feature -----------------------------------------------------------------------------------

void KeyClashFeature::init(FeatureHost& host, const Json& settings) {
    host_ = &host;
    applySettings(settings);
}

onair::OnAirConfig KeyClashFeature::onAirConfig() const {
    if (!host_) return {};
    return onair::OnAirSettings::fromJson(host_->settings("onair")).config();   // channel map only; own instance
}

void KeyClashFeature::applySettings(const Json& settings) {
    const KeyClashSettings s = KeyClashSettings::fromJson(settings);
    const onair::OnAirConfig oa = onAirConfig();            // before taking mu_ (host call)
    std::lock_guard<std::mutex> lk(mu_);
    const bool wasEnabled = s_.enabled;
    s_ = s;
    if (!s.enabled || !wasEnabled) logic_.reset();           // enabling starts clean
    logic_.configure(s, oa);
}

void KeyClashFeature::onEvent(const net::Event& ev, int64_t) {
    std::lock_guard<std::mutex> lk(mu_);
    logic_.onEvent(ev);
}

void KeyClashFeature::onTick(int64_t nowNs, int64_t) {
    if (!host_) return;
    std::vector<Notice> notices;
    KeyClashSettings s;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!s_.enabled) return;
        if (nowNs - lastOaPollNs_ >= 1'000'000'000 || lastOaPollNs_ == 0) {   // follow FS-01 channel map changes
            lastOaPollNs_ = nowNs ? nowNs : 1;
            const onair::OnAirConfig oa = onair::OnAirSettings::fromJson(host_->settings("onair")).config();
            if (oa.channelMap != logic_.onAirConfig().channelMap) logic_.configure(s_, oa);
        }
        MetaStore& meta = host_->meta();                      // leaf lock, safe under mu_
        notices = logic_.tick(nowNs, [&meta](uint32_t id) -> std::optional<std::string> {
            auto m = meta.get(id);
            if (!m || m->key.empty()) return std::nullopt;
            return m->key;
        });
        if (notices.empty()) return;
        s = s_;
    }
    out::OutputManager& o = host_->outputs();                // after mu_ is released
    if (!s.oscEnabled || !o.oscEnabled()) return;
    const out::OscProfile p = o.oscProfile();
    for (const Notice& n : notices) {
        if (p == out::OscProfile::Generic || p == out::OscProfile::MagicQ)
            o.sendOsc("/shunt/keyclash", {out::OscArg::Int(n.alarm ? 1 : 0), out::OscArg::Str(n.ref), out::OscArg::Str(n.cued)});
        else if (p == out::OscProfile::QLab && n.alarmEdge && n.alarm && s.qlabKeyClashCue > 0)
            o.sendOsc("/cue/" + std::to_string(s.qlabKeyClashCue) + "/start");
    }
}

Json KeyClashFeature::status() const {
    const size_t tracks = host_ ? host_->meta().size() : 0;
    std::lock_guard<std::mutex> lk(mu_);
    const KeySnapshot& k = logic_.snapshot();
    Json j = Json::object();
    j["level"] = k.pair ? levelName(k.level) : "none";
    j["alarm"] = k.alarm;
    auto deck = [](const DeckKey& d) {
        Json o = Json::object();
        o["deck"] = d.deck;
        o["key"] = d.text;
        return o;
    };
    j["reference"] = k.pair ? deck(k.reference) : Json();
    j["cued"] = k.pair ? deck(k.cued) : Json();
    Json src = Json::object();
    for (int d = 1; d <= kDecks; ++d)
        if (k.loaded[size_t(d)]) src[std::to_string(d)] = sourceName(k.sources[size_t(d)]);
    j["sources"] = src;
    j["strict"] = s_.strict;
    j["assumeKeyLock"] = s_.assumeKeyLock;
    j["metaTracks"] = double(tracks);
    j["experimental"] = s_.experimentalStatusKey;
    return j;
}

std::optional<HttpResponse> KeyClashFeature::handle(const HttpRequest& req, const std::string& subpath) {
    if (subpath != "settings") return std::nullopt;
    HttpResponse r;
    if (req.method != "GET" && req.method != "HEAD") {
        r.status = 405;
        r.body = "{\"ok\":false,\"error\":\"GET only\"}";
        return r;
    }
    const Json raw = host_ ? host_->settings("keyclash") : Json::object();   // no own lock held (rule 3)
    r.body = KeyClashSettings::fromJson(raw).toJson().dump();
    return r;
}

} // namespace shunt::app::keyclash

SHUNT_REGISTER_FEATURE(shunt::app::keyclash::KeyClashFeature);
