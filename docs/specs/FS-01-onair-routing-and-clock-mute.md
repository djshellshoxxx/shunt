# FS-01: On-air routing and clock mute

Status: draft 0.1. Depends on FS-00. Feature id `onair`. UI file `60-onair.js`.

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
