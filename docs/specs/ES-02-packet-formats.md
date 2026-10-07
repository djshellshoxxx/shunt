# ES-02: Packet formats (detailed engineering spec)

Status: draft 0.1, normative for the parsers and builders in `shunt_net`.
Research: `docs/research/01-pro-dj-link-protocol.md`. Offsets are absolute
from the start of the datagram; all multi-byte integers big-endian unless
stated. Column "V" records verification: D = from Deep Symmetry's published
analysis and beat-link source, C = confirmed against our own capture (to be
filled in during implementation). Nothing is normative until it is C or D.

---

## 1. Common header (all ports)

| Offset | Len | Field | Value |
|---|---|---|---|
| 0x00 | 10 | magic | `51 73 70 74 31 57 6d 4a 4f 4c` |
| 0x0a | 1 | type | port-dependent |
| 0x0b or 0x0c | 20 | device name | NUL padded; at 0x0c on port 50000, 0x0b on 50001 and 50002 |

Parser rule: reject any datagram shorter than 0x24 or with a wrong magic.
Log and count, never throw.

## 2. Port 50000: announce and claim

### 2.1 Keep-alive (type 0x06, 54 bytes) V: D

| Offset | Len | Field |
|---|---|---|
| 0x0a | 1 | 0x06 |
| 0x0b | 1 | 0x00 |
| 0x0c | 20 | name |
| 0x20 | 1 | 0x01 |
| 0x21 | 1 | kind: 0x01 CDJ, 0x02 mixer, 0x03 rekordbox |
| 0x22 | 2 | 0x0036 |
| 0x24 | 1 | device number |
| 0x25 | 1 | 0x01 CDJ, 0x02 mixer |
| 0x26 | 6 | MAC |
| 0x2c | 4 | IPv4 |
| 0x30 | 1 | peer count |
| 0x31 | 3 | 0 |
| 0x34 | 1 | 0x01 CDJ, 0x02 mixer |
| 0x35 | 1 | 0x00 legacy; **0x64 required for CDJ-3000 compatibility** |

Builder: Shunt sends this with name "Shunt", kind 0x01, our number, MAC,
IP, peer count = devices seen including self, 0x35 = 0x64.

### 2.2 Claim sequence V: D

| Type | Len | Notable fields |
|---|---|---|
| 0x0a hello | 0x25 (0x26 in CDJ-3000-compatible form) | name at 0x0c; kind at 0x20 |
| 0x00 stage 1 | 0x2c | counter N (1..3) at 0x24; MAC at 0x26 |
| 0x02 stage 2 | 0x32 | IP at 0x24, MAC at 0x28, number at 0x2e, counter at 0x2f, auto flag at 0x31 (0x01 auto, 0x02 specific) [field order per dysentery startup.adoc; verify] |
| 0x04 stage 3 | 0x2a | number at 0x24, counter at 0x25 |
| 0x08 defend | 0x29 | number at 0x24 |
| 0x01, 0x03, 0x05 | mixer assignment | parsed for completeness, not sent |

Exact byte templates are to be captured from a real player's startup
(research 01 section 1) before release; until then the templates in
beat-link's documentation are used, written from the documented layout,
not copied from its source.

## 3. Port 50001: beats and control

### 3.1 Beat (type 0x28, 96 bytes) V: D

| Offset | Len | Field | Decode |
|---|---|---|---|
| 0x21 | 1 | device number | |
| 0x24 | 4 | nextBeat ms | at +0 percent pitch |
| 0x28 | 4 | 2ndBeat ms | |
| 0x2c | 4 | nextBar ms | |
| 0x30 | 4 | 4thBeat ms | |
| 0x34 | 4 | 2ndBar ms | |
| 0x38 | 4 | 8thBeat ms | 0xffffffff = track ends first |
| 0x54 | 4 | pitch | `multiplier = value / 1048576.0` (beat-link reads 3 bytes from 0x55; reading 4 from 0x54 with the top byte 0 is equivalent) |
| 0x5a | 2 | bpm100 | 0xffff unknown |
| 0x5c | 1 | beatInBar | 1 to 4; ignore from mixers (0x21) |

`effectiveBpm = bpm100 / 100.0 * multiplier`. Minimum length 0x60.

### 3.2 Precise position (type 0x0b, 60+ bytes, CDJ-3000) V: D, offsets to verify

| Offset | Len | Field |
|---|---|---|
| 0x24 | 4 | track length s |
| 0x28 | 4 | playhead ms, signed |
| 0x2c | 4 | pitch, signed, percent x 64 |
| 0x38 | 4 | bpm10, 0xffffffff unknown |

Parsed but disabled by default (jitter).

### 3.3 Channels on air (type 0x03) V: D

- 0x2d bytes: channels 1 to 4 at 0x24 to 0x27.
- 0x35 bytes, subtype 0x03 at 0x20: channels 5 and 6 at 0x2d to 0x2e.
- 0x00 off, 0x01 on.

### 3.4 Fader start 0x02, handoff 0x26 and 0x27, sync control 0x2a V: D

Parsed into events for diagnostics; only Lead mode (v2) ever sends 0x26 or
0x2a.

## 4. Port 50002: status

### 4.1 CDJ status (type 0x0a) V: D

Minimum accepted length 0xcc. Lengths by model: 0xd0 pre-nexus, 0xd4
nexus, 0x11c / 0x124 nxs2 and XDJ-AZ, 0x11b XDJ-1000, 0x200 CDJ-3000.

| Offset | Len | Field | Decode |
|---|---|---|---|
| 0x21 | 1 | device number | |
| 0x27 | 1 | activity | |
| 0x28 | 1 | source player | |
| 0x29 | 1 | slot | 0 none, 1 CD, 2 SD, 3 USB, 4 collection, 6 streaming, 7 USB-2, 9 Beatport |
| 0x2a | 1 | track type | 0 none, 1 rekordbox, 2 unanalysed, 5 CD, 6 streaming |
| 0x2c | 4 | rekordbox id | |
| 0x32 | 2 | track number | |
| 0x7b | 1 | P1 play state | 0, 2, 3, 4, 5, 6, 7, 8, 9, 0x0e, 0x11, 0x12 (table in research 01) |
| 0x7c | 4 | firmware ASCII | |
| 0x84 | 4 | Sync_n | |
| 0x89 | 1 | F flags | 0x40 play, 0x20 master, 0x10 sync, 0x08 on air, 0x02 bpm sync |
| 0x8c | 4 | Pitch_1 | **tempo source**; same encoding as beat pitch |
| 0x92 | 2 | bpm100 | 0xffff unknown |
| 0x9d | 1 | P3 | |
| 0x9e | 1 | Mm | 0 not master, 1 master rekordbox, 2 master non-rekordbox |
| 0x9f | 1 | Mh | handoff target, 0xff none |
| 0xa0 | 4 | beat number | 0xffffffff unknown |
| 0xa4 | 2 | cue countdown beats | 0x01ff none |
| 0xa6 | 1 | beatInBar | |
| 0xc0 | 4 | Pitch_3 | |
| 0xcc | 1 | nx | 0x0f nexus, 0x1f XDJ-XZ and CDJ-3000, 0x05 older |
| 0x15c | 3 | key (CDJ-3000, len >= 0x15f) | |

Pre-nexus (len 0xd0): F byte absent; play derived from P1 in {3, 4, 7, 8,
9}; master from Mm.

### 4.2 Mixer status (type 0x29, 56 bytes) V: D

| Offset | Len | Field |
|---|---|---|
| 0x21 | 1 | device number (0x21) |
| 0x27 | 1 | F (0xf0 master, 0xd0 not; same bit masks) |
| 0x2e | 2 | bpm100 (valid only while a rekordbox source plays) |
| 0x36 | 1 | Mh |
| 0x37 | 1 | beatInBar (meaningless) |

### 4.3 rekordbox status (subtype 0x01) V: D

Length field at 0x22 is total length, not remaining. Parsed as mixer-style
for master and BPM only.

## 5. Model inference

| Name prefix | Status length | Model | Capabilities |
|---|---|---|---|
| CDJ-3000 | 0x200 | CDJ3000 | beats, precise, numbers 5 to 6, key, loops |
| CDJ-2000NXS2, CDJ-TOUR1 | 0x11c / 0x124 | NXS2 | beats |
| CDJ-2000nexus, CDJ-900nexus | 0xd4 | Nexus | beats |
| CDJ-2000, CDJ-900 | 0xd0 | PreNexus | beats partial, no F byte |
| XDJ-1000, XDJ-1000MK2, XDJ-700 | 0x11b | XDJ | beats |
| XDJ-XZ | 0x11c, two decks one IP | XZ | beats, embedded mixer |
| XDJ-AZ | 0x124 | AZ | beats, numbers 5 to 6 |
| OPUS-QUAD | special | Opus | **no beats**, status after lighting hello only, numbers 9 to 12 |
| DJM-* | 0x38 | Mixer | on air, mixer beats |
| rekordbox | varies | Rekordbox | master, BPM |

Unknown names default to NXS2 capabilities with a "unverified model"
badge.

## 6. Builders (what Shunt sends)

- Keep-alive (section 2.1), hello and claim stages (section 2.2), defend.
- Lead mode only: status 0x0a (length 0x11c, fields as a nxs2 player; F
  flags, Pitch_1 = neutral, bpm100, beat number, beatInBar, Mm) at 5 Hz, and
  beats 0x28 at the published tempo. Deferred to v2.
- Opus Quad lighting hello (kind 0x03 keep-alive with number 0x17 plus the
  lighting request on 50002) only when the user enables "Opus Quad mode",
  because it impersonates rekordbox.

## 7. Test requirements

- P-T1. Golden decode: hand-built byte arrays for every packet type decode
  to the expected struct; every field in the tables above has a test.
- P-T2. Golden encode: keep-alive and claim builders produce the documented
  lengths and the CDJ-3000 byte at 0x35.
- P-T3. Length matrix: status packets of every documented length parse;
  shorter than 0xcc are rejected; longer unknown lengths parse the common
  prefix.
- P-T4. Round trip: builders' output decodes to the input values.
- P-T5. Capture-backed tests once captures exist: each capture packet
  decodes without a parse error and the decoded tempo sequence matches the
  golden JSON.
