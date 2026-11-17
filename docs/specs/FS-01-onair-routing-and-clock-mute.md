# FS-01: On-air routing and clock mute

Status: implemented (W1). Section "As implemented" and "Deviations" at the end are the reference. Depends on FS-00. Feature id `onair`. UI file `60-onair.js`.

## Goal and users

Lighting operators and hardware users want to know which deck is audible and
want their sequencers to stop following Shunt when the DJ's mix has gone
silent (breaks, between sets), then restart cleanly on the next downbeat.

## Behaviour

**Per-deck on-air state** has two sources: the player's own F-byte on-air bit
(status packets) and the mixer's on-air packet (0x03 on port 50001, channel
bytes). Rules:

1. When a mixer has sent an on-air packet in the last 2 s, the mixer is
   authoritative: deck `d` is on air iff `channels[channelMap[d]-1] != 0`.
2. Otherwise the deck's own bit is used.
3. `channelMap` (settings) defaults to identity (`[1,2,3,4,5,6]`) because a
   player is not necessarily plugged into the mixer channel with its number;
   the UI offers a small mapping editor.

**Aggregate** `any` is true when at least one deck is on air.

**Mute policy** (`midiMute`): `off` (default) or `stop`.

- `any` goes false and stays false for `holdMs` (default 4000, range
  500..30000) then `muted = true`. Going true again unmutes immediately.
- Mute never engages if no on-air data has ever been seen since start
  (`dataSeen` false) or no player is present.
- On mute the MIDI clock sends Stop (0xFC) once and stops ticking. On unmute
  it resyncs and sends Start (`unmuteWith: start`, default) or Continue
  (`continue`) at the next bar boundary (next beat if `!barKnown`).
- If a deck leaves the network while it was the only on-air deck, `any` is
  recomputed immediately and the hold timer applies as usual.
- Link is not affected. OSC and DIN pulses follow MIDI transport semantics
  (DIN consumes the same Stop/Start stream, see FS-09).

## Settings (`features.onair`)

```json
{ "enabled": false, "holdMs": 4000, "midiMute": "off", "unmuteWith": "start",
  "channelMap": [1,2,3,4,5,6], "oscPerDeck": true }
```

Validation: `holdMs` clamped 500..30000; `channelMap` entries 1..6, wrong
length falls back to identity; unknown enum values fall back to defaults.

## Status (`status.features.onair`)

```json
{ "any": true, "muted": false, "dataSeen": true, "source": "mixer",
  "decks": [ {"deck":1,"on":true}, {"deck":2,"on":false} ], "sinceMs": 12345 }
```

## OSC

Existing `/shunt/onair i deck i state` (Generic, MagicQ) moves behind
`oscPerDeck`. New: `/shunt/onair/any i` on change, `/shunt/mute i` on mute
edges. QLab profile: optional cue numbers `qlabOnAirCue`, `qlabMuteCue`
(extra fields in `osc`, default 0 = off). Resolume and grandMA3: nothing.

## Design

`OnAirAggregator` (pure class, `app/src/features/OnAir.{h,cpp}`):

```cpp
struct OnAirConfig { int64_t holdNs; std::array<uint8_t,6> channelMap; };
class OnAirAggregator {
public:
    void configure(const OnAirConfig&);
    void onPlayerBit(uint8_t deck, bool on, int64_t nowNs);
    void onMixerChannels(const uint8_t ch[6], int count, int64_t nowNs);
    void onDeckLeft(uint8_t deck, int64_t nowNs);
    // Returns edges since last call: AnyChanged(bool), MuteChanged(bool).
    std::vector<Edge> tick(int64_t nowNs);
    bool deckOn(uint8_t deck, int64_t nowNs) const;
    bool any() const; bool muted() const; bool dataSeen() const;
};
```

`MidiClockGenerator` gains `setMuted(bool)`, `setUnmuteMode(Start|Continue)`.
Muting is orthogonal to the existing pause logic: muted wins; resume after
unmute re-enters the normal path. `OutputManager::setMute(bool)` is an atomic
flag read by the output thread at the top of each loop. The feature calls it
from `onTick` (atomic store: allowed under the Core lock).

Core change needed in `handle()`: stop writing `Live::onAir` from two
places; `Live` keeps `onAirStatus` and `onAirMixer` separately and the
device JSON uses `OnAirAggregator::deckOn`. This is done inside the feature
by reading the same events (FS-00 hook), and `Core` keeps only the legacy
`Live::onAir` display fed from the aggregator through `status()`.

## UI

- Outputs page MIDI card (slot `outputs-midi`): checkbox "Stop the clock when
  nothing is on air", hold seconds, "Restart with Start / Continue", channel
  map editor (6 selects).
- Live valve (slot `live-valve`): chip "MUTED" in amber with the time since
  mute; chip "ON AIR 1, 3" otherwise.

## Edge cases

Rekordbox peer on air bit is ignored (kind Rekordbox). Crossfader cut between
two decks (both off for <holdMs) never mutes. Passive mode: works, mixer
packets are still received. Opus Quad: no 0x03 packets, own bits only.

## Tests (`tests/features/test_onair.cpp`)

- OA-T1 hold: any false 3.9 s no mute, 4.1 s mute, true again unmutes at once.
- OA-T2 source precedence and 2 s mixer staleness fallback.
- OA-T3 channel map remap; wrong-length map falls back.
- OA-T4 no data ever: never mutes.
- OA-T5 deck leaves while on air.
- O-T8 generator mute: exactly one 0xFC, zero 0xF8 while muted, unmute emits
  one 0xFA at a bar boundary (`barKnown`) or beat boundary; Continue variant.
- OA-T6 OSC messages and QLab cues.
- OA-T7 API round trip: settings validation and status shape.
- Simulator scenario `onair` (FS-TEST section 2) end to end: clock stops at
  about t+34 s and restarts at the next bar after deck 2 goes on air.

## Shared hook delivered by this PR

`OutputManager::sendOsc(address, args)` (thread-safe, queues to the output
thread, honours the OSC profile's enabled flag) so FS-02, FS-03 and later
features send their own OSC messages without editing `Osc.cpp`. Test OA-T8:
the hook delivers a message through a mock sink and is a no-op when OSC is
disabled. See BUILD-PLAN-FEATURES section 4.

## Acceptance

All tests pass under ASan/UBSan and TSan; with `midiMute: stop` a real
byte stream captured from the MIDI port shows Stop/Start around the scripted
gap; no change in behaviour when the feature is disabled (golden status JSON).
Effort: M (3 to 4 days).

## As implemented

Files: `app/src/features/OnAir.{h,cpp}` (`OnAirAggregator`, `OnAirSettings`,
`OnAirFeature`, registered as `onair`), `ui/js/60-onair.js`,
`ui/css/60-onair.css`, `tests/features/test_onair.cpp`; shared edits in
`out/include/shunt/out/{Midi,Osc,OutputManager}.h`, `out/src/{Midi,OutputManager}.cpp`.

**Aggregator.** Decks 1..6 only. A deck is present after its first status
packet and until a `Device Left` event (or a `Joined` event of kind
Rekordbox, whose status packets are then ignored). `deckOn(d)`: present and,
while the last mixer packet is at most 2 s old, `channels[channelMap[d]-1] != 0`
(channels beyond the packet's 4 or 6 count are off); else the player's
F-byte bit. `dataSeen` becomes true on the first mixer packet or the first
player bit that is on (so a setup whose bits are always 0 never mutes).
The hold timer runs only while `midiMute` is `stop`, `dataSeen`, at least one
deck is present and `any` is false; it restarts when any of these comes back
(e.g. a player arrives). `tick(now)` returns edges `DeckChanged(deck, on)`,
`AnyChanged(on)`, `MuteChanged(muted)` in order; it allocates only when there
are edges.

**MIDI generator** (`out::MidiClockGenerator`): `setMuted(bool)`,
`setUnmuteMode(out::UnmuteMode::Start|Continue)`, `muted()`. The first poll
after muting sends one Stop (0xFC) if the clock was running (not before the
first lock, not when already stopped by `pause: stop`), then nothing. The
first poll after unmuting resyncs to the timeline (a reset during the mute
needs no SPP or extra Start) and arms Start or Continue for the next beat
with beat-in-bar 1 (next beat when `!barKnown`); clocks resume with it. When
unmuted while paused with `pause: stop`, the clock stays stopped and the
restart message on play follows the unmute mode.

**OutputManager** (all thread-safe; the first three are atomic stores, safe
under the Core lock):

```cpp
void setMute(bool muted);                 // read by the output thread each loop -> gen.setMuted
void setUnmuteMode(out::UnmuteMode m);
bool muteRequested() const;
void setLegacyOnAirOsc(bool on);          // false: Core's OutputEvent::OnAir (/shunt/onair) dropped
// OscExtras hook for FS-02, FS-03 and later features:
bool sendOsc(const std::string& address, std::vector<OscArg> args = {});
OscProfile oscProfile() const;
bool oscEnabled() const;
```

`sendOsc` validates the address (must start with `/`), returns false and
drops the message while OSC is disabled or when 256 messages are queued, and
otherwise queues it for the output thread, which sends it with
`OscOutput::send(address, args)` (new public method; sends under every
profile, still gated by the enabled flag). It takes only the OutputManager
mutex, so it may be called from feature `onEvent`/`onTick` (Core lock held)
or any other thread; features choose addresses per profile with
`oscProfile()`.

**Feature wiring.** `onEvent` feeds PlayerStatus, OnAir (0x03) and Device
events; `onTick` runs `tick`, stores `setMute(enabled && muted)` and sends
OSC for the edges after releasing its own mutex. While enabled it calls
`setLegacyOnAirOsc(false)` and sends per-deck `/shunt/onair i deck i state`
itself (Generic and MagicQ, when `oscPerDeck`) from the aggregated state
(mixer or player bits). Generic and MagicQ also get `/shunt/onair/any i` on
change and `/shunt/mute i` on mute edges; QLab gets `/cue/<qlabOnAirCue>/start`
when `any` goes true and `/cue/<qlabMuteCue>/start` on mute (0 = off);
Resolume and grandMA3 get nothing. Disabling (or `shutdown`) clears the mute
and restores the legacy per-deck messages; enabling starts with a fresh
aggregator.

**Settings** `features.onair`: `enabled` (bool, false), `holdMs` (number,
clamped 500..30000, wrong type 4000), `midiMute` (`off`|`stop`, other values
`off`), `unmuteWith` (`start`|`continue`, other `start`), `channelMap`
(array of exactly 6, else identity; each entry rounded and clamped 1..6, a
non-number entry is its identity value), `oscPerDeck` (bool, true),
`qlabOnAirCue`, `qlabMuteCue` (number clamped 0..9999, else 0).

**Status** `status.features.onair`: `any`, `muted`, `dataSeen` (bool),
`source` (`mixer`|`players`|`none`), `decks` (`[{deck, on}]` for present
decks), `sinceMs` (ms since the last `any` or mute edge, as of the last
tick), plus host keys `enabled`, `error`.

**Routes**: `GET /api/f/onair/settings` returns the validated settings
object (same keys as above); other methods 405; other subpaths 404. Settings
are written with `POST /api/config {"features":{"onair":{...}}}`.

**UI**: widget in `outputs-midi` (always shown, so the feature can be
switched on): "Follow on-air state", "Stop the clock when nothing is on air",
hold seconds, "Restart with Start / Continue", channel map editor (six
selects, BUILD-PLAN risk R5); widget in `outputs-osc`: per-deck messages and
the two QLab cues. Each has an "Apply on-air settings" button (the page's own
Save does not include features). Live valve chip (`featureId: onair`):
"ON AIR 1, 3", "OFF AIR", "NO ON-AIR DATA", or amber "MUTED m:ss".

**Tests** (`tests/features/test_onair.cpp`): OA-T1 hold, OA-T2 precedence and
staleness, OA-T3 channel map and map validation, OA-T4 no data / no player,
OA-T5 deck leaves, O-T8 generator mute (Start, Continue, bar unknown, pause
interplay, mute before lock), OA-T6 OSC per profile, oscPerDeck, QLab,
legacy gate, OA-T7 settings validation, status shape and routes through
Core, OA-T8 `sendOsc` hook (sink, disabled no-op, other thread, queue bound),
OA-T9/T9b the `onair` timeline built with `FixtureBuilder` (mixer packets,
and player bits only with Continue) fed through an unstarted `NetworkStack`,
`ClockEngine`, the feature and `MidiClockGenerator` with synthetic time: one
Stop at 34.0 s +- 0.4, no clocks while muted, restart on a downbeat within a
bar after 36 s; with `midiMute: off` no Stop at all. OA-T10 `onTick` budget.

## Deviations

1. QLab cue numbers `qlabOnAirCue`, `qlabMuteCue` live in `features.onair`,
   not in `osc` (`Settings.cpp` is frozen and features only see their own
   settings object).
2. The pure class's `OnAirConfig` has `muteEnabled` (from `midiMute`), so
   `muted` is never true with `midiMute: off` and no `/shunt/mute` is sent;
   `Edge` also has `DeckChanged(deck)` for the per-deck OSC messages.
3. `dataSeen` needs a mixer packet or at least one player bit that is on,
   not merely a status packet (bits are 0 on setups without on-air support).
4. `any` and mute are deck based: a mixer channel that is up with no Pioneer
   player mapped to it (e.g. a turntable) does not count as on air.
5. "OSC follows MIDI transport semantics" is implemented as the `/shunt/mute`
   edge message only; OSC beat messages keep running while muted, like Link.
6. Per-deck `/shunt/onair` moved from Core (mixer packets only) to the feature
   while it is enabled, so it also reflects player bits and the channel map.
   Core still writes `Live::onAir` for the device JSON (`Core.cpp` frozen);
   the aggregated per-deck view is `status.features.onair.decks`.
7. The hook is `OutputManager::sendOsc` plus `oscProfile()`/`oscEnabled()`;
   no `OscOutput::addExtra` callback registry was added (one queueing entry
   point is enough and keeps all sends on the output thread).
8. A real MIDI byte capture with hardware is not part of CI; the scenario
   test checks the generator's byte stream instead (see the acceptance list).
