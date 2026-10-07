# ES-04: Product engineering spec

Status: draft 0.1, normative. Research: `docs/research/02-market-competitors-hardware-legal.md`.

## 1. Statement

Shunt takes the beat from a Pioneer DJ / AlphaTheta player network and
diverts it, bar-accurate, into Ableton Link, MIDI clock and OSC, and writes
down exactly what was played. One native binary, no laptop with two network
cards, no Java, no lighting licence. Desktop app on macOS, Windows and
Linux; appliance on Raspberry Pi.

## 2. Users and jobs

| User | Job | Must have |
|---|---|---|
| Hybrid act (synths, drummer, Ableton next to a DJ) | play in time and on the bar with the DJ | Link with bar phase; MIDI clock DIN and USB; downbeat LED; bar offset button |
| DJ with hardware (Elektron, modular, drum machine) | hardware follows the master deck | MIDI clock that does not hunt on handoff; keep running on pause |
| Lighting or VJ operator without a show-control budget | beat, bar, BPM, track into Resolume, grandMA, MagicQ, TouchDesigner | Link, OSC profiles, MIDI clock |
| DJ, promoter, radio | exact tracklist and cue sheet after the set | network log with on-air times, CSV, CUE, text, JSON |
| Venue | permanent booth install | appliance, read-only, no NFS, web page |

## 3. Editions

| Edition | Form | Includes | Price hypothesis |
|---|---|---|---|
| Shunt Monitor | desktop, free | device list, master, BPM, beat LED, tracklist on screen, no outputs | free |
| Shunt | desktop | all outputs, tracklist export, profiles, replay | $69 |
| Shunt Box | Pi appliance image plus optional hardware | everything plus DIN out, web UI, OLED | image $59; assembled box $279 |
| Venue licence | per install | Box plus multi-year updates, CSV email export | $249 |

## 4. Features by priority

**P0 (v1.0)**

- Follow mode join with safe device number; Passive mode for coexistence
  with rekordbox.
- Master tracking with handoff; clock engine per ES-03.
- Outputs: Link (tempo plus phase, quantum 4 or 1), MIDI clock on any OS port
  plus a virtual port, OSC with Resolume, grandMA3, generic profiles
  (formats per Pacemaker ES-02, shared code).
- Bar offset control (button, MIDI, OSC) and beat-only mode.
- Latency offset per output; phase scope page.
- Tracklist log from status packets: load, on air, unload, with rekordbox
  id, deck, slot, track number; export CSV, CUE, timestamp text, NDJSON.
- Title and artist resolution via dbserver when in Lead mode with a number
  1 to 6; otherwise "ID only" with a later match against a rekordbox XML
  export the user supplies.
- Compatibility badges per device: Full, Tempo only (Opus Quad), Unsupported
  (XDJ-RX series), Unverified.
- Record and replay of sessions (pcapng).
- Desktop app: tray icon, main window with device tiles, master, timeline,
  outputs, tracklist.

**P1 (v1.x)**

- Appliance image (RS-02).
- WebSocket JSON state and OBS browser-source overlay page.
- Mixer on-air to OSC; mute clock when the master is off air (option).
- Phrase countdown from CDJ-3000 phrase data where present on the network.
- NFS metadata (opt-in, with a clear warning, off by default).

**P2 (v2)**

- Lead mode: become tempo master so the CDJs follow Link (reverse Shunt).
- StageLinQ input for Denon.
- Unofficial TCNet-compatible output after legal review.
- LTC and MTC per deck.

**Never**: Device Library Plus decryption; anything that writes to a
player; any use of "Bridge" or "certified" in naming.

## 5. UX

House style as Vivisect and Pacemaker: dark, high contrast, medical. The
main screen is a "circulation" diagram: player tiles on the left (number,
name, model badge, play state, BPM, on-air ring, master crown), the Shunt
valve in the middle showing the filtered BPM, bar:beat and confidence, and
output tiles on the right (Link peers, MIDI port and jitter, OSC
destination). A bottom strip is the tracklist with timestamps.

Controls: Bar offset (cycle 0 to 3), Beat-only, Follow or Passive, Latency
offset, Profile (Stage or Rehearsal), Record session, Export tracklist.

Compatibility page listing every model from research 02 section 4 with its
badge and reason, reachable from any tile.

## 6. Naming and legal wording

Product name "Shunt". Required wording on the website, in the app's About
box and in the README: "Shunt works with PRO DJ LINK compatible players
and mixers from AlphaTheta / Pioneer DJ. PRO DJ LINK, rekordbox and Pioneer
DJ are trademarks of their respective owners. Shunt is an independent
product and is not affiliated with, endorsed, licensed or certified by
AlphaTheta Corporation or Pioneer Corporation." No AlphaTheta logos. The
input is described as "the player network".

## 7. Success metrics for v1.0

- Lock on a real CDJ-2000NXS2 and CDJ-3000 within 2 beats; no false resets
  over a 1-hour set in a club capture.
- Link phase error at a receiving laptop under 3 ms RMS on a wired switch.
- MIDI clock jitter under 0.5 ms on macOS and Linux, under 1.5 ms on
  Windows.
- Tracklist completeness: every track that was on air for over 30 s is
  logged with the correct on-air time within 1 s.
- Zero complaints of players being kicked off the network in beta.
