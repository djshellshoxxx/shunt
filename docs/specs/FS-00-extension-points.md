# FS-00: Extension points (foundation for FS-01 to FS-09)

Status: implemented (PR 0). Section 7 is the reference as built; deviations
from the draft are listed at the end. It changes no behaviour. It exists so that
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

## 7. As implemented (reference for feature PRs)

Files: `app/include/shunt/app/Feature.h` (IFeature, FeatureHost, ReplayHooks,
FeatureRegistry, `SHUNT_REGISTER_FEATURE`), `ControlRegistry.h`,
`MetaStore.h`; Core side in `app/src/CoreFeatures.cpp`; UI core in
`ui/js/00-core.js`; UI build in `cmake/build_ui.cmake`.

**IFeature** (all optional except `id`): `id()`, `defaultEnabled()` (false),
`init(FeatureHost&, const Json& settings)`, `applySettings(const Json&)`,
`onEvent(const net::Event&, int64_t wallMs)`, `onTick(int64_t nowNs, int64_t wallMs)`,
`Json status() const`, `std::optional<HttpResponse> handle(const HttpRequest&, const std::string& subpath)`,
`shutdown()`. Register with `SHUNT_REGISTER_FEATURE(ClassName);` at namespace
scope (class default constructible). Ids: `[a-z0-9]{1,32}`; invalid or
duplicate ids are dropped with a log line.

| Call | Thread / lock | When |
|---|---|---|
| `init` | caller of `Core::start()`, no Core lock | once, alphabetical id order |
| `applySettings(features.<id>)` | HTTP thread, no Core lock | after `POST /api/config` changed `features.<id>` |
| `onEvent`, `onTick` | net thread, Core lock held | only while enabled and not in error; `onTick` every <= 5 ms with a stack or replay, 50 ms otherwise |
| `status` | any, Core lock held | every `/api/status` and WebSocket frame |
| `handle` | HTTP thread, no Core lock | `/api/f/<id>/<subpath>`, any method, raw body; also while disabled |
| `shutdown` | `Core::stop()`, no Core lock | once |

Enabled = `settings.features.<id>.enabled` (bool), else `defaultEnabled()`.
`status.features.<id>` is the feature's object plus host keys `enabled` and
`error` (empty, or the last exception text while in error state).

**Error containment**: every call is wrapped; each exception is logged to
stderr; 5 within 60 s put the feature in error state: no more
`onEvent`/`onTick`, `handle` answers 503, status shows `error`. Any change of
`features.<id>` through `POST /api/config` re-arms it. Exceptions in
`ReplayHooks::step` end the replay.

**FeatureHost**: `wallNowMs()`, `meta()`, `controls()`, `outputs()`,
`settings(id)` (copy, `{}` if absent; safe with or without the Core lock),
`saveSettings(id, value)` (replaces `features.<id>`, writes config.json,
updates enabled, does not call `applySettings`; returns false inside
Core-locked callbacks), `engine()` (only inside onEvent/onTick/status/step),
`onSessionCleared(cb)` (after `/api/session/clear`, no Core lock, once per
clear), `enterReplay(ReplayHooks*)`, `leaveReplay()`, `replaying()`, and,
only inside `ReplayHooks::step`: `feed(port, data, len, recvNs, srcIp, wallMs = 0)`,
`resetEngine(bool clearSession = false)`, `setOutputsEnabled(bool)`.

**Replay seam**: `enterReplay` (not under the Core lock; false if already
replaying or Core not started) parks the live engine, per-deck state, scope,
device list, counters and TracklistSession, installs fresh ones, turns
outputs off (`Core::timeline()` returns an empty timeline, output events are
dropped) and from the next net-loop pass stops the live stack (no sockets) and
calls `step(nowNs)` every ~5 ms with the Core lock held. `feed` parses a
datagram with an unstarted `NetworkStack` (parsers, DeviceTable,
MasterTracker), runs its timers and `engine.tick(recvNs)`, and dispatches the
events through Core and every enabled feature's `onEvent`. Replay rows are
never appended to `<data>/tracklists/`; dbserver lookups are skipped.
`leaveReplay` restores everything and rebuilds the live stack; called inside
`step` it takes effect at the end of that pass. `Core::stop()` leaves an
active replay first.

**Controls**: `ControlRegistry::add(ControlAction)` (replaces same id; empty
id or fn ignored), `remove(id)`, `has(id)`, `invoke(id, value)` (value
clamped to 0..1, NaN to 0; false if unknown or when called with the Core lock
held), `list()` (copies, sorted by id). `ControlAction{id, label, kind
(Trigger|Toggle|Value), fn(double), group}`. Built in: `baroffset.cycle`,
`baroffset.set.0..3`, `beatonly.toggle` (Toggle), `beatonly.on`,
`beatonly.off`, `latency.up`, `latency.down` (0.5 ms steps as on the Live
page), `setstart.mark`.

**MetaStore** (leaf lock, safe anywhere): `get(id) -> optional<log::TrackMeta>`,
`contains`, `size`, `snapshot()`, `replace(MetaMap)`, `set(id, meta)`,
`apply(rows)`, `load(path)`, `save(path)` (file format unchanged).

**Routes**: `GET /api/features` -> `[{id, enabled, error}]`;
`GET /api/controls` -> `[{id, label, kind, group}]`;
`POST /api/controls/<id>` `{"value":0..1}` (optional, default 1) -> `{ok}` or
404/400; `/api/f/<id>/<rest>` -> `handle(req, "<rest>")`, 404 JSON for an
unknown id or `nullopt`, 503 in error state, 500 on exception. OSC input:
`/shunt/action/<id> [f|i]` (no argument means 1).

**UI**: `ui/index.template.html` holds the shell with `/*@CSS@*/` and
`/*@JS@*/`; the build concatenates `ui/css/*.css` in name order and emits each
`ui/js/*.js` as its own `<script>` with `"use strict"` (one module that throws
cannot stop the others). Built-in modules: `00-core.js`, `10-live.js`,
`20-outputs.js`, `25-tracklist.js`, `30-scope.js`, `35-compat.js`,
`40-diagnostics.js`, `css/00-base.css`. Globals from `00-core.js`: `h()`,
`api()`, `$()`, `S` (last status), `fmtBpm`, `fmtTime`, and `Shunt`:

- `Shunt.registerPage({id, title, order, featureId?, mount(root), update(status)})`
  at load time; `id` must be the first key and a literal (the build checks
  uniqueness); built-in orders 10, 20, 25, 30, 35, 40. A page with
  `featureId` is in the nav only while that feature is enabled
  (`status.features.<id>.enabled`, initially `/api/features`).
- `Shunt.registerWidget({slot, mount(el), update(status), featureId?, order?})`,
  slots `banner` (shell, between header and main, persistent),
  `live-valve`, `live-controls`, `outputs-midi`, `outputs-osc`. Widgets of a
  disabled feature are hidden and not updated. Slot containers use
  `display: contents`, so an empty slot changes no layout.
- `Shunt.slot(name)` (used once by the built-in pages), `Shunt.h`,
  `Shunt.api`, `Shunt.featureOn(id)`, `Shunt.S`, `Shunt.pages`.

`cmake/build_ui.cmake` fails on a missing built-in module, a file name other
than `NN-name.(js|css)`, a reserved number (60 to 68) used with another id, a
`</script` inside a module, a `Shunt.registerPage(` without a literal first
`id`, or a repeated page id. ctest `ui_build_checks` covers these cases.

**Tests** (`tests/features/test_fs00_*.cpp`, ids FS00-T*): T1/T1b deep merge
and settings; T2 registry; T3/T3b built-in controls over HTTP and OSC; T4
feature registry; T5 dummy lifecycle; T6 exceptions and error state; T7/T7b
replay seam and outputs gating; T8 lock-order stress (2 s, run under TSan in
CI). Screenshot parity before/after the UI split: 0.000 % differing pixels
on all six pages with frozen data at 1440 px; under 1 % with live simulator
data.

## Deviations

1. `IFeature::defaultEnabled()` added (features whose spec says default on).
2. `FeatureHost` also has `replaying()`; `feed` takes an optional `wallMs`
   (FS-05 maps capture time to wall time); `resetEngine(bool clearSession)`.
   FS-05's draft wording (hooks supplying `feed`/`reset`/`swapSession`) is
   superseded by this section: the host supplies them, sessions are parked by
   `enterReplay` and reset by `resetEngine(true)`. FS-05 should be updated by
   its owner.
3. `ReplayHooks::step` runs with the Core lock held (same rules as
   `onEvent`); `enterReplay`/`leaveReplay` must be called without it (from
   `handle()` or a feature thread); `leaveReplay` inside `step` is deferred.
4. The host, not the feature, skips `onEvent`/`onTick` while a feature is
   disabled; host keys `enabled`/`error` are added to each status object;
   `/api/features` also returns `error`.
5. `ControlRegistry` gained `has()`; `invoke` clamps and refuses under the Core
   lock (would self-deadlock on built-in actions).
6. `/api/f/...` accepts every HTTP method (other routes still 405 for
   methods other than GET/HEAD/POST). `Http.cpp` gained reason phrases for
   201, 204, 401, 403, 409, 415, 422, 429, 503.
7. The registry singleton lives in `shunt_app` (`app/src/Feature.cpp`);
   `shunt_features` is an OBJECT library built from the glob, or from a
   generated stub when the glob is empty. Every executable that links
   `shunt_app` must also link `shunt_features` (done for `shunt_headless` and
   `ShuntTests`).
8. OSC input is now also polled when no network stack is up (before, OSC
   control only worked with a running stack); without a stack the net loop
   ticks features every 50 ms.
9. `Core.cpp` did not shrink by 150 lines: the metadata code (about 45 lines)
   moved to `MetaStore`, and the new host code went to `CoreFeatures.cpp`
   instead of growing `Core.cpp` further (573 -> 607 lines).
10. UI: modules are separate inline scripts rather than one script; boot runs
    on `DOMContentLoaded`; the `banner` slot is a `div.slot` between header
    and main; all existing CSS stays in `00-base.css`.
11. During a replay, `POST /api/session/clear` clears only the replay session,
    and a recording request takes effect when the replay ends.

