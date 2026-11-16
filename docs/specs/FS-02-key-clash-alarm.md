# FS-02: Key clash alarm

Status: implemented (W2). Depends on FS-00 (MetaStore). Feature id `keyclash`. UI `61-keyclash.js`.
Files: `app/src/features/KeyMath.{h,cpp}` (pure), `KeyClash.{h,cpp}` (KeyClashLogic + feature),
`ui/js/61-keyclash.js`, `ui/css/61-keyclash.css`, `tests/features/test_keyclash.cpp`, `tests/golden/key_levels.txt`.

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

## As implemented

Settings (`features.keyclash`, all validated and clamped, wrong types fall back to the default):
`enabled` false, `strict` false, `assumeKeyLock` true, `debounceMs` 1500 (0..30000), `oscEnabled` true,
`qlabKeyClashCue` 0 (0..9999, 0 = off), `experimentalStatusKey` false.

Status (`status.features.keyclash`, plus the host keys `enabled` and `error`): `level`
(`perfect|compatible|energy|clash|unknown|none`; `none` = no pair), `alarm`, `reference` and `cued`
(`{deck, key}`, `null` without a pair; `key` is `""` when unknown, shown after the optional pitch shift),
`sources` (`{"<deck>": "xml|status|none"}` for every deck with a loaded track), `strict`, `assumeKeyLock`,
`metaTracks` (MetaStore size; the UI uses it for the "Import rekordbox XML for keys" hint), `experimental`.

Route: `GET /api/f/keyclash/settings` (validated settings; other methods 405, other subpaths 404). Settings are
written through `POST /api/config`.

OSC: Generic and MagicQ profiles get `/shunt/keyclash i alarm s refKey s cuedKey` (via
`OutputManager::sendOsc`) whenever the alarm bit or the displayed key pair changes, including the return to
`0 "" ""`. QLab gets `/cue/<qlabKeyClashCue>/start` on the rising alarm edge only. Nothing is sent when
`oscEnabled` is false or OSC output is off.

UI: Live valve chip (green "8A to 9A compatible" / "same key", amber "energy", red "KEY CLASH 8A vs 2A" pulsing
until muted or `prefers-reduced-motion`), "Mute key alarm" button in the live controls (visible only while the
alarm is on; silences the pulse for the current pair only, in the page's memory), settings card on the Outputs
page (slot `outputs-osc`), link to the Tracklist page when no metadata is loaded.

## Deviations and unverified items

1. **Status key bytes (R1/K0) are not decoded.** `decodeStatusKey()` returns `nullopt`. The setting
   `experimentalStatusKey` (default off, no UI toggle) enables `experimentalDecodeStatusKey()`, an UNVERIFIED
   hypothesis that the three bytes hold NUL-terminated ASCII such as `"8A"`; it is only a fallback when the XML
   has no key. The `shuntsim` `key` scenario's placeholder bytes (`8a 00 00`, ...) are therefore not decoded;
   use `--key-bytes 38,41,00` to exercise the hypothesis. Needs a real CDJ-3000 capture (K0) before anyone
   relies on it.
2. **Camelot / Open Key reference.** The wheel (1A = Abm ... 12B = E; 8B = C, 8A = Am; a semitone is +7 wheel
   steps) is written from the Mixed In Key Camelot wheel (https://mixedinkey.com/harmonic-mixing-guide/) and
   the Open Key notation (1d = C major, 1m = A minor, Open Key number = Camelot number - 7; Traktor / Beatport
   convention). These pages could not be fetched from the build environment; the table was instead verified
   against an independent oracle in the tests (tonic pitch classes, relative minor = +3 semitones, a fifth =
   +1 step) for all 24 keys and all 576 pairs, plus the reviewed golden `tests/golden/key_levels.txt`.
   Open Key input (`1d`, `6m`) is accepted by `parseKey` in addition to the spec's spellings.
3. **R6 (key lock flag not parsed):** `assumeKeyLock` true ignores pitch. With false, each deck's key is shifted
   by its own `round(12*log2(pitch))` semitones (clamped to +-24).
4. **Parser details:** a bare note letter (`A`) is a major key; `M` alone is rejected as ambiguous; double
   accidentals, `13A`, `0A` and anything with trailing text are `unknown`.
5. **On-air state** is derived by a private `onair::OnAirAggregator` (same rules and the FS-01 channel map,
   re-read from `settings("onair")` once a second), not read from the FS-01 feature: FS-01 exposes no accessor
   and keyclash must also work with FS-01 disabled. Rekordbox peers are ignored as in FS-01.
6. **No MetaStore version counter exists**, so the per-id cache is omitted: the (at most six) lookups are
   redone at 5 Hz inside `onTick`, which makes a later XML import take effect within 200 ms. `onEvent` does
   not allocate in steady state (counting-allocator test K-T11); `onTick` allocates (key strings).
7. A "loaded" deck has a non-zero rekordbox id and a non-zero track type. "On air" needs the deck to be present.
   Debounce is evaluated at the 5 Hz tick, so its resolution is 200 ms.
8. `/shunt/keyclash` is also sent when the pair becomes known or unknown (not only on alarm edges), as the spec's
   "key-pair changes" implies.

## Tests (tests/features/test_keyclash.cpp)

K-T1 parse/format (24 keys, 43 spellings, garbage, fuzz), K-T2 level matrix (oracle + golden), K-T3 pitch shift,
K-T4 pair selection, K-T5 debounce/edges/strict/5 Hz, K-T6 no metadata and import without restart, K-T7 OSC
(generic, toggle, QLab), K-T8 settings validation, status shape, routes, K-T9 `key` scenario end to end through
the FixtureBuilder capture and the NetworkStack, K-T10 status-byte hypothesis opt-in and XML precedence,
K-T11 allocation and time budget, K-T12 FS-01 channel map.
