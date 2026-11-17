# FS-06: Downbeat tap, fine nudge and confidence breakdown

Status: implemented (W2). Depends on FS-00; actions are bindable through FS-04 MIDI learn. Deviations at the end.
Feature id `nudge`. UI `65-nudge.js`.

## Goal

When the player-reported bar is wrong (loops, odd edits, Opus Quad, beat-only
tracks) let the user fix it in one tap on a phone or footswitch, trim phase
in small steps, and see why confidence is low.

## 1. Tap to set the downbeat

User taps when they hear the one. Server computes the new bar offset.

- Input: `POST /api/f/nudge/tap {}`; the server stamps receipt at
  `t = now - tapLatencyMs` (setting, default 60, range 0..250, to cover
  network and finger delay).
- Preconditions (else 409 with a reason string for the UI toast): a master
  exists; `barKnown` is true or the offset is being forced (beat-only mode
  returns 409 "bar not available: turn off beat only"); state Locked or
  Coasting; confidence >= 0.5.
- Nearest beat: `k = round(beatIndex + (t - beatOriginNs) / periodNs)`;
  distance `d = |t - timeOfBeat(k)| / periodNs`. If `d > 0.35` return 409
  "tap closer to a beat".
- Packet beat in bar of beat k: `raw = (((tl.beatInBar - 1 - userOffset) +
  (k - tl.beatIndex)) mod 4 + 4) mod 4 + 1`. New offset
  `newOffset = (1 - raw) mod 4` (non-negative). Apply with
  `Core::setBarOffset` (this also stores the per-track memory).
- Response `{ok, offset, previous, raw, distanceBeats}`. `POST undo` restores
  `previous` (single level, cleared on track change or 60 s).
- Rate limit: ignore taps within 300 ms of the previous.

## 2. Fine nudge of phase

Adjusts the existing latency offset in small steps, momentary and bindable.

- `POST /api/f/nudge/step {ms}` adds `ms` (range -5..+5) to `latencyMs`,
  clamped by the global -20..+50 limit. Rounded to 0.1.
- Actions registered: `nudge.plus` (+0.5 ms), `nudge.minus`, `nudge.fine.plus`
  (+0.1), `nudge.fine.minus`, `nudge.reset` (back to the value at the last
  Save or the default 1.0), `downbeat.tap`, `downbeat.undo`.
- UI hold-to-repeat: after 400 ms repeat every 120 ms and escalate 0.1, 0.5,
  1 ms steps; releases stop at once.
- Each change is a normal `setLatencyMs`, so persistence and engine config
  follow the existing path; a small rolling log (last 20 changes with time)
  is shown for transparency.

## 3. Confidence breakdown

Expose what `updateConfidence` uses: status
`{hitRate, rmsMs, madMs, state, locked, formula}` where `formula` is the text
"0.5 hit + 0.3 jitter + 0.2 locked". The UI shows three bars, each with a
plain sentence ("7 of 8 beats arrived on time", "jitter 1.2 ms", "locked").
Needs read access to `engine.track(master)` (`hitRate()`, `rmsResidual()`,
`mad()`), available inside `status()` per FS-00.

## Settings (`features.nudge`)

```json
{ "enabled": true, "tapLatencyMs": 60, "wakeLock": true, "haptics": true }
```

## UI: Nudge pad (`#/nudge`, phone first)

Full-height layout, 3 zones: top = valve in miniature (BPM, bar lamps, state);
middle = giant TAP button (min 40 vh, `touch-action: manipulation`, fires on
`pointerdown`); bottom = `- 1  -0.1  +0.1  +1` ms buttons with hold-repeat,
Undo, and the confidence bars. Uses the Screen Wake Lock API when available
(feature-detected, silently skipped otherwise) and `navigator.vibrate(15)`
for haptics where supported. Keyboard: Space = tap, arrows = nudge. Large
text (>= 18 px), no modals, toast area with `aria-live`.

## Tests

- N-T1 offset maths: for all `userOffset` 0..3 and all `raw` 1..4 the tap on a
  beat yields `beatInBar == 1` after applying (exhaustive, 16 cases).
- N-T2 rejections: no master, beat only, low confidence, far from beat, rate limit.
- N-T3 tap latency compensation: tap 60 ms after a beat resolves to that beat.
- N-T4 undo semantics and expiry.
- N-T5 nudge clamping and rounding; actions register and invoke.
- N-T6 confidence JSON equals engine values on a replayed fixture.
- N-T7 UI smoke: page mounts, tap button posts, error toast text shown.

## Acceptance and effort

On the simulator with a deliberately wrong bar, one tap fixes the bar lamp
within one beat and survives restart through offset memory. Effort: M (3 days).

## Deviations (as implemented)

Files: `app/src/features/Nudge.{h,cpp}`, `ui/js/65-nudge.js`, `ui/css/65-nudge.css`,
`tests/features/test_nudge.cpp`, `tests/features/nudge_ui_test.js` (N-T7, Playwright, not wired into
ctest because CMake is frozen; run `node tests/features/nudge_ui_test.js --build-dir <dir>`).

1. Enabled by default (`defaultEnabled()` true, as the settings block says). No ClockEngine change was
   needed: `engine.track(master)` accessors suffice.
2. Features have no `Core` handle, so the bar offset is applied through the control registry
   (`baroffset.set.N`, same path and per-track memory as `Core::setBarOffset`). Latency is applied through
   `latency.up/down` (0.5 ms steps) unless a `latency.set` Value action exists (0..1 maps to -20..+50 ms).
   Core does not register `latency.set` yet (frozen file), so by default the server side of 0.1 ms steps is
   carried in an accumulator (five `nudge.fine.plus` presses make one 0.5 ms step) and `nudge.reset` is
   exact only to 0.5 ms. The UI avoids this: status field `fine` is false then and the +-0.1 buttons post to
   the existing `/api/latency` route (exact, as on the Live page). Seam to add: `latency.set` + rounding to
   0.1 in `Core::setLatencyMs`.
3. `engine()` is only read in onEvent/onTick/status, so `handle()` works on a snapshot taken there. The
   bar offset in effect when the timeline was published is captured in `onEvent` (Beat of the master), so a
   tap issued between an offset change and the next beat still computes the right raw beat. A snapshot older
   than 1 s answers 409 "no timing data". `handle()` takes time from an injectable monotonic clock
   (`setClock`, default `net::monotonicNowNs`).
4. Rate-limited taps answer 429 `{ok:false,ignored:true}` (the UI ignores them silently). Refusal texts:
   "no master", "no timing data", "bar not available: turn off beat only", "bar not available for this
   player", "not locked yet", "confidence too low", "tap closer to a beat", "nothing to undo".
5. Undo is also cleared on master change and when the master's track (rekordbox id + slot) changes.
6. "Value at the last Save" for `nudge.reset` is taken as the latency first seen after start (the value
   loaded from config.json, which is what the last save wrote); there is no separate Save action. Default 1.0
   when no engine data was seen yet.
7. The change log is built by watching the engine's latency each tick, so it also records changes made on
   the Live page or via `/api/latency`; newest first, 20 entries, wall-clock time.
8. Status keys: `hitRate, rmsMs, madMs, state, locked, formula` as specified, plus `confidence, jitterScore,
   hits, hitsOf, barOffset, latencyMs, fine, tapReady, tapBlocked, canUndo, tapLatencyMs, wakeLock, haptics,
   log`. (`enabled` and `error` are added by the host.) Extra routes: `POST reset`, `GET settings`.
9. Navigating directly to `#/nudge` on a fresh load bounces to Live (page visibility is decided before
   `/api/features` returns; `00-core.js` is frozen). Use the nav link.
10. ASan reports an alloc-dealloc-mismatch inside `std::stable_sort` in `FeatureRegistry::createAll` (a
    libstdc++ false positive, not FS-06 code); run with `ASAN_OPTIONS=alloc_dealloc_mismatch=0`.
