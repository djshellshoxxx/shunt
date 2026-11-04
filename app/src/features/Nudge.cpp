// FS-06 downbeat tap, fine nudge and confidence breakdown. Spec: docs/specs/FS-06-downbeat-tap-and-nudge.md.
// Original Shunt code.
#include "Nudge.h"
#include "shunt/net/Socket.h"
#include <algorithm>
#include <cmath>

namespace shunt::app::nudge {

namespace {
constexpr double kMinLatency = -20.0, kMaxLatency = 50.0;
constexpr double kCoreStep = 0.5;                 // latency.up / latency.down step in Core

double round1(double v) { return std::round(v * 10.0) / 10.0; }
int mod4(int64_t v) { return int(((v % 4) + 4) % 4); }

const char* stateName(clock::State s) {
    switch (s) {
    case clock::State::Idle: return "idle";
    case clock::State::Locking: return "locking";
    case clock::State::Locked: return "locked";
    case clock::State::Coasting: return "coasting";
    case clock::State::Paused: return "paused";
    case clock::State::MixerMaster: return "mixermaster";
    }
    return "idle";
}
} // namespace

// ---- pure helpers ------------------------------------------------------------------------------

NudgeSettings NudgeSettings::fromJson(const Json& j) {
    NudgeSettings s;
    if (!j.isObject()) return s;
    s.enabled = j.get("enabled").asBool(true);
    const Json& t = j.get("tapLatencyMs");
    if (t.type() == Json::Number && std::isfinite(t.asNumber())) s.tapLatencyMs = int(std::lround(std::clamp(t.asNumber(), 0.0, 250.0)));
    s.wakeLock = j.get("wakeLock").asBool(true);
    s.haptics = j.get("haptics").asBool(true);
    return s;
}

Json NudgeSettings::toJson() const {
    Json j = Json::object();
    j["enabled"] = enabled;
    j["tapLatencyMs"] = tapLatencyMs;
    j["wakeLock"] = wakeLock;
    j["haptics"] = haptics;
    return j;
}

int offsetForRaw(int raw) { return mod4(int64_t(1) - raw); }

TapGeometry tapGeometry(const clock::Timeline& tl, int userOffset, int64_t tapNs) {
    TapGeometry g;
    const double period = tl.periodNs();
    if (period <= 0) { g.distanceBeats = 1e9; return g; }
    g.k = int64_t(std::llround(double(tl.beatIndex) + double(tapNs - tl.beatOriginNs) / period));
    g.distanceBeats = std::fabs(double(tapNs - tl.timeOfBeat(g.k))) / period;
    g.raw = mod4(int64_t(tl.beatInBar - 1 - userOffset) + (g.k - tl.beatIndex)) + 1;
    g.newOffset = offsetForRaw(g.raw);
    return g;
}

std::string tapBlockReason(const clock::Timeline& tl, clock::State state, bool beatOnly, bool valid, int64_t ageNs) {
    if (!valid || tl.masterDevice == 0 || !tl.usable()) return "no master";
    if (ageNs > kStaleNs) return "no timing data";
    if (beatOnly) return "bar not available: turn off beat only";
    if (!tl.barKnown) return "bar not available for this player";
    if (state != clock::State::Locked && state != clock::State::Coasting) return "not locked yet";
    if (tl.confidence < kMinConfidence) return "confidence too low";
    return "";
}

// ---- feature -----------------------------------------------------------------------------------

NudgeFeature::NudgeFeature() : clock_([] { return net::monotonicNowNs(); }) {}

NudgeFeature::Result NudgeFeature::fail(int status, const std::string& msg) {
    Result r;
    r.status = status;
    r.body = Json::object();
    r.body["ok"] = false;
    r.body["error"] = msg;
    return r;
}

void NudgeFeature::init(FeatureHost& host, const Json& settings) {
    host_ = &host;
    applySettings(settings);
    registerActions();
}

void NudgeFeature::applySettings(const Json& settings) {
    const NudgeSettings s = NudgeSettings::fromJson(settings);
    std::lock_guard<std::mutex> lk(mu_);
    s_ = s;
}

void NudgeFeature::registerActions() {
    struct Def { const char* id; const char* label; int kind; double arg; };
    // kind: 0 step, 1 reset, 2 tap, 3 undo
    static const Def defs[] = {
        {"nudge.plus", "Nudge +0.5 ms", 0, 0.5},      {"nudge.minus", "Nudge -0.5 ms", 0, -0.5},
        {"nudge.fine.plus", "Nudge +0.1 ms", 0, 0.1}, {"nudge.fine.minus", "Nudge -0.1 ms", 0, -0.1},
        {"nudge.reset", "Nudge reset latency", 1, 0}, {"downbeat.tap", "Downbeat tap", 2, 0},
        {"downbeat.undo", "Downbeat undo", 3, 0},
    };
    for (const Def& d : defs) {
        ControlAction a;
        a.id = d.id;
        a.label = d.label;
        a.kind = ControlAction::Trigger;
        a.group = "Nudge";
        const int kind = d.kind;
        const double arg = d.arg;
        a.fn = [this, kind, arg](double) {
            { std::lock_guard<std::mutex> lk(mu_); if (!s_.enabled) return; }
            switch (kind) {
            case 0: doStep(arg, false); break;
            case 1: doReset(); break;
            case 2: doTap(); break;
            default: doUndo(); break;
            }
        };
        host_->controls().add(std::move(a));
        actionIds_.push_back(d.id);
    }
}

void NudgeFeature::shutdown() {
    if (!host_) return;
    for (const auto& id : actionIds_) host_->controls().remove(id);
    actionIds_.clear();
}

// Reads the engine: only from onEvent / onTick / status (Core lock held).
void NudgeFeature::refreshLocked(int64_t nowNs) {
    const clock::ClockEngine& e = host_->engine();
    snap_.valid = true;
    snap_.atNs = nowNs;
    snap_.tl = e.timeline();
    snap_.state = e.state();
    snap_.cfgOffset = e.config().userBarOffset & 3;
    snap_.beatOnly = e.config().beatOnlyMode;
    snap_.latencyMs = e.config().latencyOffsetNs / 1e6;
    if (!snap_.offsetKnown) { snap_.offsetAtPublish = snap_.cfgOffset; snap_.offsetKnown = true; }
}

void NudgeFeature::onEvent(const net::Event& ev, int64_t) {
    std::lock_guard<std::mutex> lk(mu_);
    switch (ev.type) {
    case net::EventType::Beat:
        if (ev.device == host_->engine().master() && ev.beat.isMaster) {
            refreshLocked(ev.recvTimeNs);
            snap_.offsetAtPublish = snap_.cfgOffset;     // the timeline was just published with this offset
            snap_.offsetKnown = true;
        }
        break;
    case net::EventType::MasterChanged:
        refreshLocked(ev.recvTimeNs);
        snap_.offsetAtPublish = snap_.cfgOffset;
        undo_.valid = false;
        trackKey_ = 0;
        break;
    case net::EventType::PlayerStatus:
        if (ev.device == host_->engine().master() && ev.device != 0) {
            const uint64_t key = (uint64_t(ev.status.rekordboxId) << 8) | ev.status.slot;
            if (key != trackKey_) { trackKey_ = key; undo_.valid = false; }
        }
        break;
    default: break;
    }
}

void NudgeFeature::onTick(int64_t nowNs, int64_t wallMs) {
    std::lock_guard<std::mutex> lk(mu_);
    lastNowNs_ = nowNs;
    refreshLocked(nowNs);
    if (!latKnown_) { latKnown_ = true; latSeen_ = snap_.latencyMs; baseline_ = snap_.latencyMs; }
    else if (std::fabs(snap_.latencyMs - latSeen_) > 1e-6) {
        log_.push_back({wallMs, latSeen_, snap_.latencyMs});
        while (log_.size() > kLogSize) log_.pop_front();
        latSeen_ = snap_.latencyMs;
    }
    if (undo_.valid && nowNs - undo_.atNs > kUndoExpiryNs) undo_.valid = false;
}

Json NudgeFeature::status() const {
    const clock::ClockEngine& e = host_->engine();
    Json j = Json::object();
    const uint8_t m = e.master();
    double hitRate = 0, rmsMs = 0, madMs = 0, jitterScore = 0;
    int hits = 0, hitsOf = 0;
    if (m != 0) {
        const clock::PlayerTrack& tr = e.track(m);
        hitRate = tr.hitRate();
        rmsMs = tr.rmsResidual() / 1e6;
        madMs = tr.mad() / 1e6;
        jitterScore = std::exp(-(rmsMs / 8.0) * (rmsMs / 8.0));
        hitsOf = std::min(tr.hitCount, 8);
        hits = int(std::lround(hitRate * hitsOf));
    }
    j["hitRate"] = hitRate;
    j["rmsMs"] = rmsMs;
    j["madMs"] = madMs;
    j["state"] = stateName(e.state());
    j["locked"] = e.state() == clock::State::Locked;
    j["formula"] = "0.5 hit + 0.3 jitter + 0.2 locked";
    j["confidence"] = double(e.timeline().confidence);
    j["jitterScore"] = jitterScore;
    j["hits"] = hits;
    j["hitsOf"] = hitsOf;
    std::lock_guard<std::mutex> lk(mu_);
    j["barOffset"] = e.config().userBarOffset & 3;
    j["latencyMs"] = e.config().latencyOffsetNs / 1e6;
    j["fine"] = host_->controls().has("latency.set");
    const std::string block = tapBlockReason(snap_.tl, snap_.state, snap_.beatOnly, snap_.valid, -1);
    j["tapReady"] = block.empty();
    j["tapBlocked"] = block;
    j["canUndo"] = undo_.valid && lastNowNs_ - undo_.atNs <= kUndoExpiryNs;
    j["tapLatencyMs"] = s_.tapLatencyMs;
    j["wakeLock"] = s_.wakeLock;
    j["haptics"] = s_.haptics;
    Json lg = Json::array();
    for (auto it = log_.rbegin(); it != log_.rend(); ++it) {
        Json o = Json::object();
        o["t"] = double(it->wallMs);
        o["from"] = it->from;
        o["to"] = it->to;
        lg.push(o);
    }
    j["log"] = lg;
    return j;
}

// ---- operations (HTTP thread or action caller; no Core lock, no own lock across host calls) -----

NudgeFeature::Result NudgeFeature::doTap() {
    if (host_->replaying()) return fail(409, "replay is running");
    const int64_t now = clock_();
    Snap s;
    int latencyMs;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (hasTap_ && now - lastTapNs_ < kRateLimitNs) {
            Result r = fail(429, "tap ignored (too fast)");
            r.body["ignored"] = true;
            return r;
        }
        hasTap_ = true;
        lastTapNs_ = now;
        s = snap_;
        latencyMs = s_.tapLatencyMs;
    }
    const std::string block = tapBlockReason(s.tl, s.state, s.beatOnly, s.valid, now - s.atNs);
    if (!block.empty()) return fail(409, block);
    const TapGeometry g = tapGeometry(s.tl, s.offsetAtPublish, now - int64_t(latencyMs) * 1'000'000);
    if (g.distanceBeats > kMaxBeatDistance) return fail(409, "tap closer to a beat");
    {
        std::lock_guard<std::mutex> lk(mu_);
        undo_.valid = true;
        undo_.previous = s.cfgOffset;
        undo_.atNs = now;
        undo_.trackKey = trackKey_;
        snap_.cfgOffset = g.newOffset;
    }
    if (!host_->controls().invoke("baroffset.set." + std::to_string(g.newOffset), 1.0)) {
        std::lock_guard<std::mutex> lk(mu_);
        undo_.valid = false;
        snap_.cfgOffset = s.cfgOffset;
        return fail(500, "bar offset action unavailable");
    }
    Result r;
    r.body = Json::object();
    r.body["ok"] = true;
    r.body["offset"] = g.newOffset;
    r.body["previous"] = s.cfgOffset;
    r.body["raw"] = g.raw;
    r.body["distanceBeats"] = g.distanceBeats;
    return r;
}

NudgeFeature::Result NudgeFeature::doUndo() {
    const int64_t now = clock_();
    int prev;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!undo_.valid || now - undo_.atNs > kUndoExpiryNs || (trackKey_ != 0 && undo_.trackKey != trackKey_)) {
            undo_.valid = false;
            return fail(409, "nothing to undo");
        }
        prev = undo_.previous;
        undo_.valid = false;
        snap_.cfgOffset = prev;
    }
    if (!host_->controls().invoke("baroffset.set." + std::to_string(prev), 1.0)) return fail(500, "bar offset action unavailable");
    Result r;
    r.body = Json::object();
    r.body["ok"] = true;
    r.body["offset"] = prev;
    return r;
}

// Applies a latency change. With a `latency.set` Value control the target is exact (0.1 ms);
// otherwise only latency.up / latency.down (0.5 ms) exist and the remainder is carried in pending_.
NudgeFeature::Result NudgeFeature::applyLatency(double target, double cur) {
    target = std::clamp(round1(target), kMinLatency, kMaxLatency);
    double result = cur;
    bool quantised = false;
    if (host_->controls().has("latency.set")) {
        host_->controls().invoke("latency.set", (target - kMinLatency) / (kMaxLatency - kMinLatency));
        result = target;
    } else {
        int steps;
        {
            std::lock_guard<std::mutex> lk(mu_);
            pending_ += target - cur;
            steps = int(pending_ / kCoreStep);
            pending_ -= steps * kCoreStep;
        }
        for (int i = 0; i < std::abs(steps); ++i) host_->controls().invoke(steps > 0 ? "latency.up" : "latency.down", 1.0);
        result = std::clamp(round1(cur + steps * kCoreStep), kMinLatency, kMaxLatency);
        quantised = true;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        snap_.latencyMs = result;
    }
    Result r;
    r.body = Json::object();
    r.body["ok"] = true;
    r.body["latencyMs"] = result;
    r.body["target"] = target;
    r.body["quantised"] = quantised;
    r.body["applied"] = std::fabs(result - cur) > 1e-9;
    return r;
}

NudgeFeature::Result NudgeFeature::doStep(double ms, bool) {
    ms = round1(std::clamp(std::isfinite(ms) ? ms : 0.0, -5.0, 5.0));
    double cur;
    {
        std::lock_guard<std::mutex> lk(mu_);
        cur = snap_.latencyMs;
    }
    return applyLatency(cur + ms, cur);
}

NudgeFeature::Result NudgeFeature::doReset() {
    double cur, base;
    {
        std::lock_guard<std::mutex> lk(mu_);
        cur = snap_.latencyMs;
        base = latKnown_ ? baseline_ : 1.0;
        pending_ = 0;
    }
    return applyLatency(base, cur);
}

std::optional<HttpResponse> NudgeFeature::handle(const HttpRequest& req, const std::string& subpath) {
    HttpResponse r;
    auto send = [&](const Result& x) { r.status = x.status; r.body = x.body.dump(); return r; };
    if (subpath == "settings") {
        if (req.method != "GET" && req.method != "HEAD") return send(fail(405, "GET only"));
        r.body = NudgeSettings::fromJson(host_ ? host_->settings("nudge") : Json::object()).toJson().dump();
        return r;
    }
    const bool known = subpath == "tap" || subpath == "undo" || subpath == "step" || subpath == "reset";
    if (!known) return std::nullopt;
    if (req.method != "POST") return send(fail(405, "POST only"));
    if (!host_) return send(fail(503, "not initialised"));
    { std::lock_guard<std::mutex> lk(mu_); if (!s_.enabled) return send(fail(409, "nudge is disabled")); }
    if (subpath == "tap") return send(doTap());
    if (subpath == "undo") return send(doUndo());
    if (subpath == "reset") return send(doReset());
    Json body;
    if (!req.body.empty() && !Json::parse(req.body, body)) return send(fail(400, "invalid JSON"));
    const Json& ms = body.get("ms");
    if (ms.type() != Json::Number || !std::isfinite(ms.asNumber())) return send(fail(400, "need ms (-5..5)"));
    return send(doStep(ms.asNumber(), true));
}

} // namespace shunt::app::nudge

SHUNT_REGISTER_FEATURE(shunt::app::nudge::NudgeFeature);
