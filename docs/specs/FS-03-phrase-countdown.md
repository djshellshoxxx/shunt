# FS-03: Phrase countdown

Status: draft 0.1. Depends on FS-00. Feature id `phrase`. UI `62-phrase.js`.
Needs hardware for full validation. Build last (wave 3).

## Goal

Show and send "bars until the next section" (intro, verse, chorus, drop,
outro) so live players know when the drop lands.

## Data sources

Phrase data is not in the packets Shunt parses today. Two sources, in order:

**A. Offline phrase maps (implementable now).** rekordbox analysis files
(`ANLZ*.EXT`) contain a PSSI tag with the song structure: a list of phrases,
each with a start beat and a kind (mood decides the label set). The user
supplies their own files. Shunt never reads a player's USB or NFS (2025
advisory; RS-04 rules).

- New tool `shunt_anlz` (`tools/shunt_anlz/main.cpp`): `shunt_anlz FILE.EXT
  --track-id N` prints phrase JSON (below). `shunt_anlz DIR --map ids.csv`
  converts many (`path,trackId` per line).
- Parser `AnlzPssi.cpp` follows Deep Symmetry's public ANLZ documentation:
  find the `PSSI` section, detect whether the payload is XOR-masked (newer
  rekordbox) and unmask, read mood, entry count, entry beat and kind. The
  implementer must verify field offsets against at least three real `.EXT`
  files (one per mood) before coding the table; do not trust this paragraph
  for offsets. Spike P0 (1 day) produces `docs/research/04-pssi-format.md`
  with verified offsets and 3 anonymised fixtures.
- Kind labels: high mood: intro, up, down, chorus, outro. Mid and low mood:
  intro, verse 1..6, bridge, chorus, outro. Stored as normalised strings
  `intro|verse|bridge|up|down|chorus|outro`.

**B. Network phrase packets (CDJ-3000, Opus lighting packets).** Out of scope
until captures exist. Task N0: capture a CDJ-3000 pair loading analysed
tracks and report whether phrase data is on port 50002 or a dbserver query.
If found, a new source feeds the same `PhraseMap`.

### Phrase map JSON (import and storage)

```json
{ "trackId": 1001, "mood": "high", "totalBeats": 512,
  "phrases": [ {"beat":1,"kind":"intro"}, {"beat":65,"kind":"up"}, {"beat":129,"kind":"chorus"} ] }
```

Stored in `<data>/phrases.json` (array). Import: `POST /api/f/phrase/import`
(one object or an array, max 2 MB), replaces entries with the same trackId.
`GET /api/f/phrase/list` returns `[{trackId, mood, phrases:n}]`;
`DELETE` via `POST /api/f/phrase/delete {trackId}`.

## Runtime

Per player deck keep `{trackId, slot, beatNumber, lastBeatNumberNs, playing}`
from status (`beatNumber` at 0xa0, 1-based, `0xffffffff` unknown) and beat
packets (increment on each beat packet of that deck, resync on each status).

For the deck's `PhraseMap`: `idx = last phrase with beat <= beatNumber`;
`next = phrases[idx+1]` (or `totalBeats+1` as the outro end); 
`beatsToNext = next.beat - beatNumber`;
`barsToNext = ceil(beatsToNext / 4.0)` and `beatsInBar` remainder for a
fractional display. Jumps (hot cue, loop exit, seek) are handled
automatically because the status resync recomputes from `beatNumber`. While
looping the beat number does not advance past the loop, so the countdown
holds (correct). Track change (id or slot change) resets state. Missing map
gives state `nomap`. Opus Quad (no beat numbers) gives `nodata`.

Summary deck for OSC and the valve: the master if it has a map, else the
first on-air deck with a map.

## Settings (`features.phrase`)

```json
{ "enabled": false, "warnBars": [8,4,2,1], "oscEnabled": true }
```

## Status

```json
{ "deck": 1, "kind": "up", "next": "chorus", "barsToNext": 3, "beatsToNext": 9,
  "phraseIndex": 1, "phraseCount": 7, "state": "ok",
  "decks": [ {"deck":1,"state":"ok","barsToNext":3}, {"deck":2,"state":"nomap"} ] }
```

## OSC

`/shunt/phrase i deck s kind s next i barsToNext` on each bar change;
`/shunt/phrase/warn i deck i bars s next` when `barsToNext` first equals a
value in `warnBars` (fires once per phrase per value, never when seeking
backwards then forwards over the same boundary within the same phrase).
QLab: optional cue `qlabPhraseCue` fired at the last warn value.

## UI

Live valve: a second ring (outer, thin) filling toward the next section with
the big number "3 bars to CHORUS"; below the bar lamps. Stage mode shows the
countdown at the same scale as the BPM. Tracklist page: "Phrase maps" card
with the list, import (file input accepting the JSON, plus a link to docs
for `shunt_anlz`), delete. Slot `live-valve` and a page section.

## Tests

- PH-T1 countdown math on a synthetic map for every beat of a 4-phrase track.
- PH-T2 jump back and forward, loop hold, track change reset.
- PH-T3 warn semantics (once per value per phrase, none after seek back and
  forth).
- PH-T4 import validation: unsorted beats get sorted, duplicates dropped,
  beat 0 or negative rejected, > 512 phrases rejected.
- PH-T5 PSSI parser on the three verified fixtures (masked and unmasked).
- PH-T6 OSC and status shape. PH-T7 persistence across restart.
- Simulator `phrase` scenario: decks send increasing `beatNumber` with a
  loaded map; expect warns at 8, 4, 2, 1.

## Acceptance and effort

P0 spike doc merged before any parser code; countdown correct to the bar on a
real CDJ-3000 with an analysed track (hardware session); no per-event
allocation. Effort: L (5 to 7 days incl. spike), highest risk of the nine.
