// Core as feature host (FS-00 sections 1 to 3): feature lifecycle with exception containment,
// settings and status wiring, built-in control actions, and the replay seam. Original Shunt code.
#include "CoreInternal.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>

namespace shunt::app {

namespace {
int64_t nowWallMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
constexpr int kMaxErrors = 5;
constexpr int64_t kErrorWindowNs = 60'000'000'000LL;

HttpResponse jsonError(int status, const std::string& msg) {
    Json j = Json::object();
    j["ok"] = false;
    j["error"] = msg;
    HttpResponse r;
    r.status = status;
    r.body = j.dump();
    return r;
}

bool enabledFrom(const Json& featureSettings, bool def) {
    const Json& e = featureSettings.get("enabled");
    return e.type() == Json::Bool ? e.asBool() : def;
}
} // namespace

// ---- built-in controls (FS-00 section 3) -------------------------------------------------------

void Core::registerControls() {
    auto add = [this](std::string id, std::string label, ControlAction::Kind kind, std::string group, std::function<void(double)> fn) {
        ControlAction a;
        a.id = std::move(id); a.label = std::move(label); a.kind = kind; a.group = std::move(group); a.fn = std::move(fn);
        controls_.add(std::move(a));
    };
    add("baroffset.cycle", "Bar offset: next", ControlAction::Trigger, "Bar offset", [this](double) { cycleBarOffset(); });
    for (int i = 0; i < 4; ++i)
        add("baroffset.set." + std::to_string(i), "Bar offset " + std::to_string(i), ControlAction::Trigger, "Bar offset",
            [this, i](double) { setBarOffset(i); });
    add("beatonly.toggle", "Beat only: toggle", ControlAction::Toggle, "Beat only", [this](double) {
        bool cur;
        { std::lock_guard<std::mutex> lk(mu_); cur = settings_.beatOnly; }
        setBeatOnly(!cur);
    });
    add("beatonly.on", "Beat only: on", ControlAction::Trigger, "Beat only", [this](double) { setBeatOnly(true); });
    add("beatonly.off", "Beat only: off", ControlAction::Trigger, "Beat only", [this](double) { setBeatOnly(false); });
    auto latencyStep = [this](double d) {
        double cur;
        { std::lock_guard<std::mutex> lk(mu_); cur = settings_.latencyMs; }
        setLatencyMs(std::round((cur + d) * 10) / 10);   // same 0.5 ms step and rounding as the Live page
    };
    add("latency.up", "Latency +0.5 ms", ControlAction::Trigger, "Latency", [latencyStep](double) { latencyStep(0.5); });
    add("latency.down", "Latency -0.5 ms", ControlAction::Trigger, "Latency", [latencyStep](double) { latencyStep(-0.5); });
    add("setstart.mark", "Set start marker", ControlAction::Trigger, "Tracklist", [this](double) { markSetStart(); });
}

Json Core::controlsList() {
    Json a = Json::array();
    for (auto& c : controls_.list()) {
        Json o = Json::object();
        o["id"] = c.id; o["label"] = c.label; o["kind"] = controlKindName(c.kind); o["group"] = c.group;
        a.push(o);
    }
    return a;
}

// ---- lifecycle ---------------------------------------------------------------------------------

bool Core::addFeature(std::unique_ptr<IFeature> f) {
    if (!f || featuresCreated_) return false;
    extraFeatures_.push_back(std::move(f));
    return true;
}

void Core::createFeatures() {
    if (featuresCreated_) return;
    featuresCreated_ = true;
    std::vector<std::unique_ptr<IFeature>> list = FeatureRegistry::instance().createAll();
    for (auto& e : extraFeatures_) list.push_back(std::move(e));
    extraFeatures_.clear();
    std::vector<std::pair<std::string, std::unique_ptr<IFeature>>> named;
    for (auto& f : list) {
        std::string id;
        try { id = f->id(); } catch (...) { id.clear(); }
        if (!validFeatureId(id)) { std::fprintf(stderr, "feature: invalid id '%s' ignored\n", id.c_str()); continue; }
        named.emplace_back(id, std::move(f));
    }
    std::stable_sort(named.begin(), named.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::set<std::string> seen;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& n : named) {
            if (!seen.insert(n.first).second) { std::fprintf(stderr, "feature: duplicate id '%s' ignored\n", n.first.c_str()); continue; }
            auto slot = std::make_unique<FeatureSlot>();
            slot->id = n.first;
            slot->f = std::move(n.second);
            slot->enabled = enabledFrom(settings_.raw.get("features").get(slot->id), slot->f->defaultEnabled());
            features_.push_back(std::move(slot));
        }
    }
    for (auto& s : features_) {            // alphabetical, no Core lock (features may call the host)
        try { s->f->init(*host_, host_->settings(s->id)); }
        catch (const std::exception& e) { featureFailed(*s, "init", e.what()); }
        catch (...) { featureFailed(*s, "init", "unknown exception"); }
    }
    featuresReady_.store(true, std::memory_order_release);
}

void Core::shutdownFeatures() {
    if (!featuresReady_.load(std::memory_order_acquire)) return;
    for (auto& s : features_) {
        try { s->f->shutdown(); }
        catch (const std::exception& e) { featureFailed(*s, "shutdown", e.what()); }
        catch (...) { featureFailed(*s, "shutdown", "unknown exception"); }
    }
}

void Core::featureFailed(FeatureSlot& f, const char* where, const std::string& what) {
    const int64_t now = net::monotonicNowNs();
    bool tripped = false;
    {
        std::lock_guard<std::mutex> lk(f.errMu);
        while (!f.errNs.empty() && now - f.errNs.front() > kErrorWindowNs) f.errNs.pop_front();
        f.errNs.push_back(now);
        ++f.errors;
        f.lastError = std::string(where) + ": " + what;
        if (int(f.errNs.size()) >= kMaxErrors && !f.failed.exchange(true)) tripped = true;
    }
    std::fprintf(stderr, "feature %s: exception in %s: %s%s\n", f.id.c_str(), where, what.c_str(),
                 tripped ? " (disabled after 5 exceptions in a minute)" : "");
}

bool Core::featureEnabledLocked(const FeatureSlot& f) const { return f.enabled && !f.failed.load(); }

#define SHUNT_GUARD(slot, where, call)                                                                  \
    try { call; }                                                                                       \
    catch (const std::exception& e_) { featureFailed(slot, where, e_.what()); }                          \
    catch (...) { featureFailed(slot, where, "unknown exception"); }

void Core::featuresOnEventLocked(const net::Event& ev, int64_t wallMs) {
    if (!featuresReady_.load(std::memory_order_acquire)) return;
    for (auto& s : features_) {
        if (!featureEnabledLocked(*s)) continue;
        SHUNT_GUARD(*s, "onEvent", s->f->onEvent(ev, wallMs))
    }
}

void Core::featuresOnTickLocked(int64_t nowNs, int64_t wallMs) {
    if (!featuresReady_.load(std::memory_order_acquire)) return;
    for (auto& s : features_) {
        if (!featureEnabledLocked(*s)) continue;
        SHUNT_GUARD(*s, "onTick", s->f->onTick(nowNs, wallMs))
    }
}

Json Core::featuresStatusLocked() {
    Json all = Json::object();
    if (!featuresReady_.load(std::memory_order_acquire)) return all;
    for (auto& s : features_) {
        Json st = Json::object();
        SHUNT_GUARD(*s, "status", st = s->f->status())
        if (!st.isObject()) { Json v = st; st = Json::object(); st["value"] = v; }
        st["enabled"] = s->enabled;
        { std::lock_guard<std::mutex> lk(s->errMu); st["error"] = s->failed ? s->lastError : std::string(); }
        all[s->id] = st;
    }
    return all;
}

std::vector<std::pair<Core::FeatureSlot*, Json>> Core::syncFeatureSettingsLocked(const Json& before) {
    std::vector<std::pair<FeatureSlot*, Json>> changed;
    if (!featuresReady_.load(std::memory_order_acquire)) return changed;
    const Json& now = settings_.raw.get("features");
    for (auto& s : features_) {
        const Json& cur = now.get(s->id);
        if (cur.dump() == before.get(s->id).dump()) continue;
        s->enabled = enabledFrom(cur, s->f->defaultEnabled());
        { std::lock_guard<std::mutex> lk(s->errMu); s->errNs.clear(); s->lastError.clear(); }
        s->failed = false;                 // applying settings again re-arms a feature in error state
        changed.emplace_back(s.get(), cur.isObject() ? cur : Json::object());
    }
    return changed;
}

Json Core::features() {
    Json a = Json::array();
    if (!featuresReady_.load(std::memory_order_acquire)) return a;
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& s : features_) {
        Json o = Json::object();
        o["id"] = s->id;
        o["enabled"] = s->enabled;
        { std::lock_guard<std::mutex> el(s->errMu); o["error"] = s->failed ? s->lastError : std::string(); }
        a.push(o);
    }
    return a;
}

std::optional<HttpResponse> Core::featureRequest(const std::string& id, const std::string& subpath, const HttpRequest& req) {
    if (!featuresReady_.load(std::memory_order_acquire) || !validFeatureId(id)) return std::nullopt;
    for (auto& s : features_) {
        if (s->id != id) continue;
        if (s->failed) return jsonError(503, "feature " + id + " is disabled after repeated errors; save its settings to re-enable");
        try { return s->f->handle(req, subpath); }
        catch (const std::exception& e) { featureFailed(*s, "handle", e.what()); }
        catch (...) { featureFailed(*s, "handle", "unknown exception"); }
        return jsonError(500, "feature " + id + " failed");
    }
    return std::nullopt;
}

// ---- FeatureHost ------------------------------------------------------------------------------

int64_t Core::HostImpl::wallNowMs() const { return nowWallMs(); }

Json Core::HostImpl::settings(const std::string& featureId) {
    auto read = [&] {
        const Json& v = c_.settings_.raw.get("features").get(featureId);
        return v.isObject() ? v : Json::object();
    };
    if (coreLockHeldByThisThread()) return read();   // already inside a Core-locked callback
    std::lock_guard<std::mutex> lk(c_.mu_);
    return read();
}

bool Core::HostImpl::saveSettings(const std::string& featureId, const Json& value) {
    if (coreLockHeldByThisThread()) {
        std::fprintf(stderr, "feature %s: saveSettings refused inside a Core-locked callback (FS-00 rule 5)\n", featureId.c_str());
        return false;
    }
    if (!validFeatureId(featureId)) return false;
    std::lock_guard<std::mutex> lk(c_.mu_);
    c_.settings_.raw["features"][featureId] = value.isObject() ? value : Json::object();
    if (c_.featuresReady_.load(std::memory_order_acquire))
        for (auto& s : c_.features_)
            if (s->id == featureId) s->enabled = enabledFrom(c_.settings_.raw.get("features").get(featureId), s->f->defaultEnabled());
    return c_.settings_.save(c_.configPath_);
}

void Core::HostImpl::onSessionCleared(std::function<void()> cb) {
    if (!cb) return;
    std::lock_guard<std::mutex> lk(c_.cbMu_);
    c_.sessionCleared_.push_back(std::move(cb));
}

bool Core::HostImpl::replaying() const {
    if (coreLockHeldByThisThread()) return c_.replay_ != nullptr;
    std::lock_guard<std::mutex> lk(c_.mu_);
    return c_.replay_ != nullptr;
}

bool Core::HostImpl::feed(uint16_t port, const uint8_t* data, size_t len, int64_t recvNs, uint32_t srcIp, int64_t wallMs) {
    if (!coreLockHeldByThisThread()) return false;   // only inside ReplayHooks::step
    return c_.feedLocked(port, data, len, recvNs, srcIp, wallMs);
}

void Core::HostImpl::resetEngine(bool clearSession) {
    if (!coreLockHeldByThisThread()) return;
    c_.resetEngineLocked(clearSession);
}

void Core::HostImpl::setOutputsEnabled(bool on) {
    if (coreLockHeldByThisThread()) { if (c_.replay_) c_.outputsEnabled_ = on; return; }
    std::lock_guard<std::mutex> lk(c_.mu_);
    if (c_.replay_) c_.outputsEnabled_ = on;      // live mode always drives outputs
}

// ---- replay seam --------------------------------------------------------------------------------

bool Core::enterReplay(ReplayHooks* hooks) {
    if (!hooks || coreLockHeldByThisThread() || !running_) return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (replay_) return false;
    parked_ = std::make_unique<Parked>();
    Parked& p = *parked_;
    p.engine = engine_; p.session = std::move(session_); p.live = std::move(live_); p.scope = std::move(scope_);
    p.tl = tl_; p.devices = std::move(devices_); p.counters = counters_;
    p.masterTrackId = masterTrackId_; p.masterTrackSlot = masterTrackSlot_;
    p.writtenEvents = writtenEvents_; p.writtenRows = writtenRows_;
    p.claimState = claimState_; p.claimNumber = claimNumber_;
    replay_ = hooks;                       // the net thread tears the live stack down on its next pass
    outputsEnabled_ = false;
    leaveReplayRequested_ = false;
    claimState_ = "replay";
    claimNumber_ = 0;
    resetEngineLocked(true);
    return true;
}

void Core::leaveReplay() {
    if (coreLockHeldByThisThread()) { leaveReplayRequested_ = true; return; }   // inside step(): defer
    std::lock_guard<std::mutex> lk(mu_);
    leaveReplayLocked();
}

void Core::leaveReplayLocked() {
    if (!replay_) return;
    replay_ = nullptr;
    replayStack_.reset();
    leaveReplayRequested_ = false;
    outputsEnabled_ = true;
    if (parked_) {
        Parked& p = *parked_;
        engine_ = p.engine; session_ = std::move(p.session); live_ = std::move(p.live); scope_ = std::move(p.scope);
        tl_ = p.tl; devices_ = std::move(p.devices); counters_ = p.counters;
        masterTrackId_ = p.masterTrackId; masterTrackSlot_ = p.masterTrackSlot;
        writtenEvents_ = p.writtenEvents; writtenRows_ = p.writtenRows;
        claimState_ = p.claimState; claimNumber_ = p.claimNumber;
        parked_.reset();
    }
    applyEngineConfig();
    restart_ = true;                       // the net thread rebuilds the live stack
}

void Core::resetEngineLocked(bool clearSession) {
    net::StackConfig cfg;
    cfg.mode = net::Mode::Follow;
    cfg.filterSubnet = false;              // never started: parsers and device table only, no sockets
    replayStack_ = std::make_unique<net::NetworkStack>(*factory_, cfg);
    engine_ = clock::ClockEngine();
    applyEngineConfig();
    live_.clear();
    scope_.clear();
    tl_ = clock::Timeline{};
    devices_.clear();
    counters_ = net::Counters{};
    masterTrackId_ = 0;
    masterTrackSlot_ = 0;
    if (clearSession) session_ = log::TracklistSession();
}

bool Core::feedLocked(uint16_t port, const uint8_t* data, size_t len, int64_t recvNs, uint32_t srcIp, int64_t wallMs) {
    if (!replay_ || !replayStack_) return false;
    const bool produced = replayStack_->feed(port, data, len, recvNs, srcIp);
    replayStack_->runTimers(recvNs);
    engine_.tick(recvNs);
    const int64_t wall = wallMs ? wallMs : nowWallMs();
    net::Event ev;
    while (replayStack_->pop(ev)) handle(ev, wall);
    tl_ = engine_.timeline();
    return produced;
}

void Core::replayPassLocked(int64_t nowNs, int64_t wallMs) {
    if (stack_) { stack_->stop(); stack_.reset(); stackUp_ = false; }   // only the net thread touches the stack
    if (replay_ && !leaveReplayRequested_) {
        try { replay_->step(nowNs); }
        catch (const std::exception& e) { std::fprintf(stderr, "replay: exception in step: %s (leaving replay)\n", e.what()); leaveReplayRequested_ = true; }
        catch (...) { std::fprintf(stderr, "replay: exception in step (leaving replay)\n"); leaveReplayRequested_ = true; }
    }
    if (replayStack_) {
        tl_ = engine_.timeline();
        devices_ = replayStack_->devices().devices();
        counters_ = replayStack_->counters();
    }
    featuresOnTickLocked(nowNs, wallMs);
    if (leaveReplayRequested_) leaveReplayLocked();
}

} // namespace shunt::app
