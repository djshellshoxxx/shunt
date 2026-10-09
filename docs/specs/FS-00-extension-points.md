# FS-00: Extension points (foundation for FS-01 to FS-09)

Status: draft 0.1. This is PR 0. It changes no behaviour. It exists so that
nine features can be built in parallel without touching the same lines.
Every later feature adds files; none edits `Core.cpp`, `Api.cpp`,
`Settings.cpp`, `CMakeLists.txt` or `ui/index.html` again.

## 1. Feature interface

`app/include/shunt/app/Feature.h`

```cpp
struct FeatureHost {                       // services; thread-safe unless noted
    virtual int64_t wallNowMs() const = 0;
    virtual MetaStore& meta() = 0;                          // section 4
    virtual ControlRegistry& controls() = 0;                // section 3
    virtual out::OutputManager& outputs() = 0;
    virtual Json settings(const std::string& featureId) = 0;        // copy
    virtual bool saveSettings(const std::string& featureId, const Json& value) = 0; // persists config.json
    virtual const clock::ClockEngine& engine() const = 0;   // ONLY valid inside onEvent/onTick/status
    virtual void onSessionCleared(std::function<void()> cb) = 0;   // FS-08: called after /api/session/clear
    virtual bool enterReplay(ReplayHooks* hooks) = 0;       // FS-05; false if already replaying
    virtual void leaveReplay() = 0;
    virtual ~FeatureHost() = default;
};

class IFeature {
public:
    virtual ~IFeature() = default;
    virtual std::string id() const = 0;                     // [a-z0-9]+ ; also the settings/status/API key
    virtual void init(FeatureHost&, const Json& settings) {}
    virtual void applySettings(const Json& settings) {}     // after validation, any thread, own locking
    virtual void onEvent(const net::Event&, int64_t wallMs) {}   // net thread, Core lock HELD
    virtual void onTick(int64_t nowNs, int64_t wallMs) {}        // net thread, Core lock HELD, every <=5 ms; rate-limit yourself
    virtual Json status() const { return Json::object(); }       // Core lock HELD; goes under status.features.<id>
    virtual std::optional<HttpResponse> handle(const HttpRequest&, const std::string& subpath) { return std::nullopt; } // HTTP thread, NO Core lock
    virtual void shutdown() {}
};
```

Registration: `SHUNT_REGISTER_FEATURE(ClassName)` at the bottom of the
feature's own `.cpp` (a static factory pusher in a registry singleton).
Because static libraries drop unreferenced objects, `shunt_app` features are
built as a CMake `OBJECT` library (`shunt_features`) linked into
`shunt_app` and the tests. Features are created in `Core::start()` in
alphabetical id order; each is constructed disabled-safe (does nothing until
`settings.enabled` is true unless the spec says default on).

### Locking contract (most likely source of bugs)

1. Lock order is `Core::mu_` then a feature's own mutex, never the reverse.
2. `onEvent`, `onTick`, `status` run with `Core::mu_` held: no blocking, no
   file or socket I/O, no calls to public `Core` methods (they would
   self-deadlock). Use `FeatureHost` only.
3. `handle()` runs without `Core::mu_`. It may call `FeatureHost` and public
   `Core` methods but must NOT hold its own mutex while doing so (that
   inverts rule 1).
4. `OutputManager` never calls back into `Core` or a feature.
5. File I/O (persisting) happens in `handle()` or a feature-owned thread,
   never in the net-thread callbacks.

### Replay hooks (for FS-05, defined here so Core is touched once)

```cpp
struct ReplayHooks {                       // implemented by the replay feature
    virtual ~ReplayHooks() = default;
    virtual void step(int64_t nowNs) = 0;  // called by the net loop instead of stack.poll while active
};
// Core side, behind enterReplay(): stops the live stack (no sockets), parks the live
// TracklistSession and installs a fresh one, creates a new ClockEngine and DeviceTable,
// exposes feed(port,data,len,recvNs,srcIp) / resetEngine() / setOutputsEnabled(bool) to the
// feature through FeatureHost, and reverses everything in leaveReplay().
```

## 2. Settings, status, API, UI wiring (done once)

| Concern | Rule |
|---|---|
| Settings | each feature owns `settings.raw.features.<id>`; `Settings::fromJson` is changed to **deep merge** (`Json::merge(patch)`: objects merge key by key, `null` deletes a key, arrays and scalars replace). Today a patch `{"features":{"a":{}}}` would replace the whole `features` object and erase other features; PR 0 fixes that with tests |
| Status | `Core::status()` adds `features: { <id>: feature.status() }` |
| HTTP | `/api/f/<id>/<rest>` is routed to `feature.handle(req, "<rest>")`; unknown id or `nullopt` gives 404 JSON. `Api.cpp` is not edited again |
| Features list | `GET /api/features` returns `[{id, enabled}]` so the UI can hide pages of disabled features |
| Banner | the shell has a `banner` slot (above `<main>`); features add persistent banners (replay, key clash alarm) as widgets |
| Tests | `tests/features/test_<id>.cpp`, picked up by `file(GLOB ... CONFIGURE_DEPENDS)` |
| CMake | `app/src/features/*.cpp` globbed with `CONFIGURE_DEPENDS` |

## 3. Control registry (actions from UI, OSC, MIDI)

```cpp
struct ControlAction {
    std::string id;                       // "baroffset.cycle", "nudge.plus", "profile.apply:<slug>"
    std::string label;
    enum Kind { Trigger, Toggle, Value } kind;   // Value: 0..1 normalised
    std::function<void(double value)> fn; // must be thread-safe; called on the caller's thread
    std::string group;                    // UI grouping
};
class ControlRegistry {
public:
    void add(ControlAction);              // replaces same id
    void remove(const std::string& id);
    bool invoke(const std::string& id, double value);   // false if unknown
    std::vector<ControlAction> list() const;            // copies
};
```

PR 0 registers the existing controls: `baroffset.cycle`, `baroffset.set.0..3`,
`beatonly.toggle`, `beatonly.on`, `beatonly.off`, `latency.up`, `latency.down`,
`setstart.mark`. Existing OSC input (`/shunt/baroffset i`, `/shunt/beatonly i`)
keeps working and gains `/shunt/action/<id> [f]` which calls `invoke`.
`GET /api/controls` lists actions; `POST /api/controls/<id>` with
`{"value":0..1}` invokes one (used by the UI and by tests).

## 4. MetaStore

`log::MetaMap meta_` moves out of `Core` into `MetaStore` (own mutex):
`get(id) -> optional<TrackMeta>`, `snapshot() -> MetaMap`, `replace(MetaMap)`,
`load(path)`, `save(path)`. `Core::tracklist/exportTracklist` use it.
Features read metadata only through `FeatureHost::meta()`.

## 5. UI modules

`ui/index.html` is split; the embed step concatenates, so the served page is
still one file.

```
ui/index.template.html     shell: header, <main>, placeholders /*@CSS@*/ /*@JS@*/
ui/css/00-base.css ...     sorted, concatenated
ui/js/00-core.js           helpers h(), api(), state S, router, Shunt.registerPage
ui/js/10-live.js ... 40-diagnostics.js   existing pages moved verbatim
ui/js/60-<id>.js ... 68    one file per feature, numbers reserved below
ui/overlay.html            unchanged
```

`Shunt.registerPage({id, title, order, featureId?, mount(root), update(status)})`.
The nav is built from the registry; a page with `featureId` is hidden when
`/api/features` says that feature is disabled. A feature may also register
`Shunt.registerWidget({slot:'live-valve'|'live-controls'|'outputs-midi'|'outputs-osc', mount, update})`
so it can add chips and controls to existing pages without editing them.
Existing pages call `Shunt.slot('live-valve')` etc. once (added in PR 0).

Reserved numbers: onair 60, keyclash 61, phrase 62, midilearn 63, replay 64,
nudge 65, profiles 66, report 67, pulse 68. CSS lives in the same-numbered
`ui/css/6x-<id>.css`.

`cmake/build_ui.cmake` builds the page from the template and fails the build
if a registered file is missing or a `Shunt.registerPage` id repeats.

## 6. PR 0 acceptance

- Behaviour identical: all existing tests pass unchanged; screenshots of all
  six pages match before and after within 1 percent pixel difference.
- New tests: deep merge (6 cases), registry add/replace/invoke/unknown,
  feature lifecycle with a dummy feature (init, settings, status, route),
  lock-order test that runs a dummy feature's `handle()` against a hammering
  `onEvent` for 2 s under TSan, UI build fails on duplicate page id.
- `Core.cpp` loses about 150 lines (meta moves out); no feature code yet.
- Replay seam covered by a dummy hooks test (enter, step called, leave restores live stack and parked session); session-cleared callback fires once.
- Documented in RS-07 section 12 (one paragraph pointing here).
