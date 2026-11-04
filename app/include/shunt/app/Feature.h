// Feature extension points (FS-00 section 1): IFeature, FeatureHost, ReplayHooks and the
// static feature registry. Every FS-01..FS-09 feature is one IFeature registered with
// SHUNT_REGISTER_FEATURE in its own app/src/features/<Name>.cpp. Original Shunt code.
//
// Locking contract (FS-00 section 1, BUILD-PLAN section 7):
//  1. Lock order is Core::mu_ then a feature's own mutex, never the reverse.
//  2. onEvent, onTick, status and ReplayHooks::step run with Core::mu_ held: no blocking, no
//     file or socket I/O, no public Core calls. Use FeatureHost only.
//  3. handle() runs without Core::mu_. It may call FeatureHost and public Core methods but must
//     not hold its own mutex while doing so.
//  4. OutputManager never calls back into a feature.
//  5. File I/O happens in handle() or a feature-owned thread, never in net-thread callbacks.
// The host wraps every call in try/catch; 5 exceptions within 60 s put the feature in the
// "error" state (no more onEvent/onTick; handle() answers 503) until its settings are applied again.
#pragma once
#include "shunt/app/ControlRegistry.h"
#include "shunt/app/Http.h"
#include "shunt/app/Json.h"
#include "shunt/app/MetaStore.h"
#include "shunt/clock/ClockEngine.h"
#include "shunt/net/Packets.h"
#include "shunt/out/OutputManager.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace shunt::app {

// Implemented by the replay feature (FS-05). step() is called by the net loop, with the Core
// lock held, instead of polling the live stack while a replay is active.
struct ReplayHooks {
    virtual ~ReplayHooks() = default;
    virtual void step(int64_t nowNs) = 0;
};

// Services offered to features. Thread-safe unless noted.
struct FeatureHost {
    virtual int64_t wallNowMs() const = 0;
    virtual MetaStore& meta() = 0;                                   // leaf lock, safe anywhere
    virtual ControlRegistry& controls() = 0;                         // add/remove anywhere; invoke not under the Core lock
    virtual out::OutputManager& outputs() = 0;
    virtual Json settings(const std::string& featureId) = 0;        // copy of settings.features.<id> (object, maybe empty)
    // Replaces settings.features.<id> and persists config.json. Does NOT call applySettings.
    // Refused (false) inside Core-locked callbacks (file I/O, rule 5).
    virtual bool saveSettings(const std::string& featureId, const Json& value) = 0;
    virtual const clock::ClockEngine& engine() const = 0;           // ONLY valid inside onEvent/onTick/status/step
    virtual void onSessionCleared(std::function<void()> cb) = 0;    // called after /api/session/clear, no Core lock held

    // ---- replay seam (FS-05) ----
    // enterReplay: stops the live stack (no sockets), parks the live engine, device state and
    // TracklistSession, installs fresh ones and turns outputs off. False if already replaying
    // or called with the Core lock held. leaveReplay restores everything and rebuilds the
    // live stack; inside step() it is deferred to the end of the current net-loop pass.
    virtual bool enterReplay(ReplayHooks* hooks) = 0;
    virtual void leaveReplay() = 0;
    virtual bool replaying() const = 0;
    // The following are ONLY valid inside ReplayHooks::step (net thread, Core lock held).
    // feed: parse one datagram as if received on <port>, run the stack timers and the engine at
    // recvNs, and dispatch resulting events to Core and to features (wallMs stamps the session;
    // 0 means now). Returns true when the datagram produced an event.
    virtual bool feed(uint16_t port, const uint8_t* data, size_t len, int64_t recvNs, uint32_t srcIp, int64_t wallMs = 0) = 0;
    // New ClockEngine and DeviceTable, per-deck live state cleared; clearSession also installs
    // a fresh replay TracklistSession.
    virtual void resetEngine(bool clearSession = false) = 0;
    virtual void setOutputsEnabled(bool on) = 0;                    // default off while replaying

    // ---- whole-config access (FS-07 profiles; optional, defaults keep test hosts minimal) ----
    // hasCoreConfig(): true when config()/applyConfig() reach the real Core settings.
    // config(): copy of the full settings document (Settings::toJson). No Core lock needed.
    // applyConfig(): POST /api/config semantics (deep merge, validation, restarts, feature
    // applySettings). Refused (false) inside Core-locked callbacks; call from handle() or a
    // feature thread without holding the feature's own mutex (lock rules 2 and 3).
    virtual bool hasCoreConfig() const { return false; }
    virtual Json config() { return Json::object(); }
    virtual bool applyConfig(const Json& /*patch*/, std::string& error) { error = "not supported by this host"; return false; }
    // Directory of config.json and the --data directory; "" when unknown.
    virtual std::string configDir() const { return ""; }
    virtual std::string dataDir() const { return ""; }

    virtual ~FeatureHost() = default;
};

class IFeature {
public:
    virtual ~IFeature() = default;
    virtual std::string id() const = 0;                              // [a-z0-9]+ ; also the settings/status/API key
    virtual bool defaultEnabled() const { return false; }            // when settings.features.<id>.enabled is absent
    virtual void init(FeatureHost&, const Json& /*settings*/) {}     // Core::start(), no Core lock, alphabetical id order
    virtual void applySettings(const Json& /*settings*/) {}          // after POST /api/config changed features.<id>; no Core lock
    virtual void onEvent(const net::Event&, int64_t /*wallMs*/) {}  // net thread, Core lock HELD, only while enabled
    virtual void onTick(int64_t /*nowNs*/, int64_t /*wallMs*/) {}   // net thread, Core lock HELD, every <=5 ms while enabled
    virtual Json status() const { return Json::object(); }           // Core lock HELD; goes under status.features.<id>
    virtual std::optional<HttpResponse> handle(const HttpRequest&, const std::string& /*subpath*/) { return std::nullopt; } // HTTP thread, NO Core lock
    virtual void shutdown() {}                                       // Core::stop(), no Core lock
};

// ---- registry ----------------------------------------------------------------------------------

using FeatureFactory = std::function<std::unique_ptr<IFeature>()>;

class FeatureRegistry {
public:
    static FeatureRegistry& instance();       // the one global mutable object (BUILD-PLAN section 7)
    void add(FeatureFactory f);
    std::vector<std::unique_ptr<IFeature>> createAll() const;   // sorted by id
    size_t size() const;

private:
    mutable std::mutex mu_;
    std::vector<FeatureFactory> factories_;
};

struct FeatureRegistrar {
    explicit FeatureRegistrar(FeatureFactory f) { FeatureRegistry::instance().add(std::move(f)); }
};

bool validFeatureId(const std::string& id);   // [a-z0-9]{1,32}

// True while this thread runs inside a Core-locked section (feature callbacks). Used to refuse
// calls that would self-deadlock or do I/O under the lock.
bool coreLockHeldByThisThread();
struct CoreLockedScope {                      // set by Core around its locked sections
    CoreLockedScope();
    ~CoreLockedScope();
    CoreLockedScope(const CoreLockedScope&) = delete;
    CoreLockedScope& operator=(const CoreLockedScope&) = delete;
private:
    bool prev_;
};

} // namespace shunt::app

#define SHUNT_FEATURE_CAT2(a, b) a##b
#define SHUNT_FEATURE_CAT(a, b) SHUNT_FEATURE_CAT2(a, b)
// Place at namespace scope at the bottom of the feature's .cpp. The class must be default constructible.
#define SHUNT_REGISTER_FEATURE(ClassName) \
    static ::shunt::app::FeatureRegistrar SHUNT_FEATURE_CAT(shunt_feature_registrar_, __LINE__)( \
        [] { return std::unique_ptr<::shunt::app::IFeature>(new ClassName()); })
