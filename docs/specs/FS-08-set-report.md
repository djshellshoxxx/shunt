# FS-08: Set report

Status: draft 0.1. Depends on FS-00 (MetaStore); uses FS-01 optionally.
Feature id `report`. UI `67-report.js`.

## Goal

After a set, one page the DJ, promoter or sound engineer can keep or print:
what was played, how the mix went, how well Shunt tracked it. It is also the
best demo of the tracklist feature.

## Contents

1. **Header:** venue, performer, date, start and end (first on-air or session
   start to now/clear), duration.
2. **Tracklist:** rows with start time, deck, artist - title (metadata or
   "ID"), key and BPM when known, played percent.
3. **Tempo:** min, median, max master BPM; sparkline (inline SVG) of master
   BPM per 10 s.
4. **Handoffs:** count and times of master changes and which decks.
5. **Sync quality:** time share in Locked, Coasting, Paused, Locking; reset
   count; per-minute phase RMS (ms) with median and 95th percentile;
   confidence below 0.5 minutes count.
6. **Outputs:** MIDI messages sent and RMS jitter (last), OSC messages sent,
   Link peers max, mute periods (FS-01) when available.
7. **Network:** devices seen with model and badge; parse errors; warnings
   ("Opus Quad: tempo only", "unverified model").
8. **Footer:** Shunt version and the required legal sentence (LEGAL.md).

No IP or MAC addresses anywhere in the output.

## Collection (`SetStats`, pure class, `app/src/features/SetStats.{h,cpp}`)

Fed by feature callbacks, 1 Hz sampling in `onTick` (no per-event cost):

- `sample(nowNs, state, bpm, confidence, resetSeq, master)` once a second:
  state seconds counters, 10 s BPM bucket median, confidence-low counter,
  reset counter (increments on `resetSequence` change), master changes.
- `residual(ms)` per master beat: per-minute vectors capped at 150 values
  (drop oldest) giving RMS and p95 per minute.
- `outputs(...)` snapshot of counters every 10 s.
- Serialised to `<data>/reports/<session>.stats.json` every 30 s by a
  feature-owned writer triggered from `handle()`-side timer thread (never
  from the net callbacks); loaded at start if the same session file exists.

A "set" equals a tracklist session: it starts with the session and ends on
`POST /api/session/clear` (the existing endpoint; the feature observes it
through a `FeatureHost` session-cleared callback added in FS-00) or process
stop. Closed sets are kept on disk; list shows the last 50.

## API (`/api/f/report/`)

| Route | Result |
|---|---|
| `GET list` | `[{id, start, end, tracks, durationS}]` (current set has id `current`) |
| `GET json?id=` | the full report object |
| `GET html?id=` | self-contained printable HTML (one file, inline CSS and SVG, no scripts, no external URLs), `Content-Disposition` attachment with `?download=1` |
| `POST delete {id}` | removes a closed set's files; cannot delete `current` |

`id` is `[A-Za-z0-9-]` only (validated), never a path.

## HTML rules

All user-derived text (venue, performer, titles, artists) is escaped for
HTML; a golden test includes `<script>alert(1)</script>` and `"` and `&` in a
title. Print stylesheet: A4 portrait, white background, page breaks avoid
splitting rows. Colours also readable in greyscale. Charts are SVG with text
alternatives.

## UI

Page `#/report`: report list on the left, preview on the right (the JSON
rendered with the same components), buttons Download HTML and Print (calls
`window.print()` on a print-only view). A small "Set summary" widget on the
Tracklist page shows tracks, duration and sync quality of the current set.

## Settings (`features.report`)

```json
{ "enabled": true, "keepSets": 50 }
```

## Tests

- RP-T1 `SetStats` aggregation with synthetic seconds (state shares add to
  100 percent, bucket medians, reset counting).
- RP-T2 p95 and RMS from known residual vectors; cap behaviour.
- RP-T3 persistence round trip and crash recovery (truncated file ignored).
- RP-T4 HTML golden file for a small fixed set (`tests/golden/report_small.html`;
  regenerate with `UPDATE_GOLDEN=1`, diff reviewed in PR).
- RP-T5 escaping test (script tag, quotes, ampersand, unicode).
- RP-T6 no `http://`, `https://` (except in the legal text if it has none, so
  none), no `<script`, no IP or MAC regex matches in output.
- RP-T7 id validation and delete rules. RP-T8 clear-session closes the set.

## Acceptance and effort

A replayed 20 minute fixture (FS-05) produces a report whose numbers equal
independently computed values within rounding; HTML opens offline and prints
to one or two pages. Effort: M (4 days).
