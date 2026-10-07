# RS-04: Tracklist logging (regular spec)

Status: draft 0.1. Research: `docs/research/03-timing-appliance-integrations.md` section 5, `docs/research/01-pro-dj-link-protocol.md` section 6.

## Events

From status packets: `Loaded(deck, rekordboxId, slot, trackType,
trackNumber, t)`, `OnAir(deck, t)` from F bit 3 rising or the 0x03 on-air
packet, `OffAir(deck, t)`, `Unloaded(deck, t)`, `Master(deck, t)`.

A row is "played" when the deck was on air or master for over 30 s or over
20 percent of the track length (when known). Start time is the first on-air
moment, not the load.

## Metadata resolution

1. Lead mode with number 1 to 6: dbserver query for title, artist, album,
   genre, key, BPM, duration, label (cached by id and slot).
2. Else, offline match: the user drops a rekordbox XML export; ids are
   matched to it after the set.
3. NFS (Crate Digger method): P1, opt-in, with a warning referencing the
   2025 advisory, off by default.
4. Opus Quad and XDJ-AZ: id only; no decryption.

## Storage

NDJSON per session in `/data/tracklists/` (Box) or the documents folder
(desktop), one object per event and per played row, appended with fsync.
Rows: `{startedAt, endedAt, deck, slot, rekordboxId, trackNumber, title,
artist, album, key, bpm, durationS, playedPct, source}`.

## Exports

| Format | Notes |
|---|---|
| CSV | Serato history columns: `#, name, artist, album, genre, bpm, key, start time, end time, played, deck, rekordbox_id, source` |
| CUE | `PERFORMER`, `TITLE`, `FILE "mix.wav" WAVE`, `TRACK nn AUDIO`, `INDEX 01 mm:ss:ff` at 75 fps relative to the "set start" marker |
| Timestamp text | `[hh:mm:ss] Artist - Title`; unknowns as `ID - ID` |
| NDJSON | the raw session |
| Performance report CSV | `date, venue, title, artist, writer(s), publisher, ISRC, duration`, writer and ISRC left blank |

## Overlay

`/overlay` page on the local HTTP server (desktop and Box) for OBS browser
source: now playing, next, BPM; WebSocket-driven. obs-websocket text update
as an option.
