# FS-02: Key clash alarm

Status: draft 0.1. Depends on FS-00 (MetaStore). Feature id `keyclash`. UI `61-keyclash.js`.

## Goal

Warn, before the DJ brings a track in, when the cued deck's key will clash
with what is on air. Useful to hybrid acts playing keys over the DJ.

## Key sources

1. **rekordbox XML** `Tonality` via `MetaStore` (primary; exists after the
   existing import). Accepted spellings: musical (`Am`, `F#m`, `Bb`, `A minor`,
   `C♯ major`) and Camelot (`8A`, `12B`). Anything else is "unknown".
2. **Status bytes 0x15c..0x15e** (CDJ-3000, `PlayerStatusEvent::key`).
   Their encoding is not documented in this repo. Task K0 (spike, 0.5 day):
   capture a CDJ-3000 loading tracks of known keys and decode; until then
   `decodeStatusKey()` returns `nullopt`. The feature must work with source 1
   alone.

## Normalisation and compatibility

Internal form: Camelot `(number 1..12, letter A|B)`. Conversion table (both
directions) is a constant in `KeyMath.cpp` and is covered by a round-trip test
of all 24 keys plus enharmonic spellings.

Pitch shift: if `assumeKeyLock` is false, shift the key by
`round(12*log2(pitchMultiplier))` semitones (Camelot number `+7` per semitone,
mod 12). Default `assumeKeyLock = true` (club practice), so pitch is ignored.

Levels between key A (reference) and B (cued):

| Level | Rule |
|---|---|
| `perfect` | same key |
| `compatible` | same letter, number +-1; or same number, other letter |
| `energy` | same letter, number +-2 |
| `clash` | everything else |
| `unknown` | either key missing |

## Which decks

Reference deck: the master if on air, else the lowest-numbered on-air deck
(uses FS-01 aggregator when enabled, else each deck's own on-air bit). Cued
deck: among decks with a loaded track that are not on air, the one currently
playing, else the lowest number. No pair means level `none`.

## Alarm

Alarm is raised when level is `clash` (or `energy` when `strict` is true) for
at least `debounceMs` (default 1500), and clears when the level improves for
the same time. Alarm edges are events for OSC.

## Settings (`features.keyclash`)

```json
{ "enabled": false, "strict": false, "assumeKeyLock": true, "debounceMs": 1500,
  "oscEnabled": true }
```

## Status

```json
{ "level": "clash", "alarm": true, "reference": {"deck":1,"key":"8A"},
  "cued": {"deck":2,"key":"3B"}, "sources": {"1":"xml","2":"xml"} }
```

## OSC

`/shunt/keyclash i alarm s refKey s cuedKey` on alarm edges and key-pair
changes (QLab: optional cue `qlabKeyClashCue`).

## UI

Live valve chip: green "8A to 9A compatible", amber "energy", red pulsing
"KEY CLASH 8A vs 3B" (pulse stops with reduced motion). Live control bar:
mute-alarm button (silences the visual pulse for the current pair only).
Outputs page: strictness, key lock assumption. If no metadata is loaded the
chip says "Import rekordbox XML for keys" with a link to the Tracklist page.

## Design

`KeyMath` (pure): `parseKey(string) -> optional<Camelot>`, `toString`,
`shift(Camelot, semitones)`, `level(a, b)`. `KeyClash` feature: on each
status event update per-deck `{trackId, slot, onAir, playing, pitch}`; compute
pair in `onTick` at 5 Hz; key lookups through `meta().get(id)`; the lookup
result is cached per `(id)` and invalidated by a `MetaStore` version counter.

## Tests

- K-T1 parse/format round trip for all 24 keys, 40 spellings, garbage input.
- K-T2 level matrix for all 24 x 24 pairs against a table written by hand
  from the rules (generated once, reviewed, committed as a golden table).
- K-T3 pitch shift with `assumeKeyLock=false`.
- K-T4 pair selection (master on air, two cued decks, none).
- K-T5 debounce and edges, strict mode.
- K-T6 no metadata gives `unknown`, import later updates without restart.
- K-T7 OSC edges. K-T8 API/status shape.

## Acceptance and effort

Table test exhaustive and reviewed; XML import then alarm works in the
simulator `key` scenario (FS-TEST); no allocation in `onEvent` hot path.
Effort: S to M (2 to 3 days; +0.5 day if K0 finds the encoding).
