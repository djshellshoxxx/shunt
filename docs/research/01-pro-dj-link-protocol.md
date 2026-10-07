# Research 01: Pro DJ Link protocol (network and packet level)

Status: research notes, 7 October 2026. Primary input to
`docs/specs/ES-01-network-stack.md` and `docs/specs/ES-02-packet-formats.md`.
The canonical Deep Symmetry analysis site was blocked by the research
proxy, so the **same AsciiDoc sources were read from the dysentery and
beat-link-trigger GitHub repos**, plus the beat-link Java sources directly
(VirtualCdj, BeatFinder, TimeFinder, CdjStatus, MixerStatus, Util,
DeviceFinder, DeviceAnnouncement, VirtualRekordbox, OpusProvider,
ConnectionManager, CHANGELOG). Pioneer PDFs and forums were blocked; those
points are from secondary sources and flagged. **All multi-byte integers are
big-endian unless stated.** Every offset below must be verified against a
real capture before being treated as normative; the packet spec records
which have been.

---

## 1. Transport, discovery and device-number assignment

**Ports (all UDP, IPv4):**

| Port | Role | Direction |
|---|---|---|
| 50000 | announce, device-number negotiation, keep-alive | broadcast (255.255.255.255 or subnet broadcast); mixer-assignment packets unicast |
| 50001 | beats, precise position, fader start, channels on air, sync control, master handoff | beats and on-air broadcast; handoff and sync control unicast to the target |
| 50002 | CDJ status (0x0a), mixer status (0x29), media query (0x05/0x06), load track (0x19/0x1a), load settings (0x34) | **unicast** to every device that has announced itself |
| 50004 | touch audio (0x1e/0x1f/0x20) | irrelevant |
| 12523/TCP | "RemoteDBServer" port discovery | unicast |
| 1051/TCP (typical) | dbserver metadata | unicast |
| 2049/UDP | NFSv2 (Crate Digger path) | unicast |

No multicast is used; everything is broadcast or unicast on one L2
segment.

**Common header:** every packet starts with the 10-byte magic
`51 73 70 74 31 57 6d 4a 4f 4c` ("Qspt1WmJOL"), byte 0x0a = packet type
(meaning depends on port: 0x0a is "hello" on 50000 but "CDJ status" on
50002), then a 20-byte NUL-padded device name. For 50001 and 50002 packets:
0x20 = subtype (0x00 CDJ, 0x01 when rekordbox sends mixer-style status, 0x03
for the six-channel on-air variant), 0x21 = device number D, 0x22 to 0x23 =
`len_r` (bytes remaining after this field; rekordbox's subtype-01 packets put
total length here instead). For 50000 packets the name is at 0x0c and the
layout differs.

**Port 50000 packet types** (type, length, cadence):

| Type | Name | Length | Cadence |
|---|---|---|---|
| 0x0a | initial hello | 0x25 (37) | 3 times at about 300 ms |
| 0x00 | first-stage claim (counter N = 1..3 at 0x24) | 0x2c (44) | 3 times at about 300 ms |
| 0x01 | mixer assignment intention (mixer to player, unicast) | 0x2f | |
| 0x02 | second-stage claim (carries IP and MAC, auto-assign flag at 0x1f: 0x01 auto, 0x02 specific) | 0x32 (50) | 3 times |
| 0x03 | mixer channel assignment (mixer to player) | 0x27 | |
| 0x04 | final-stage claim | 0x2a (42) | 3 times |
| 0x05 | mixer assignment finished | 0x26 | |
| 0x06 | keep-alive | 0x36 (54) | CDJs about 1.5 to 2 s, mixers about 1.5 s; beat-link default 1500 ms (allowed 200 to 2000) |
| 0x08 | channel conflict / defend | 0x29 (41) | on demand |
| 0x10 | "rekordbox lighting hello" sent by all-in-ones (Opus Quad) | | |

**Keep-alive (0x06, 54 bytes) layout** (beat-link `keepAliveBytes` and
`DeviceAnnouncement` offsets):

| Offset | Len | Field |
|---|---|---|
| 0x00 | 10 | magic |
| 0x0a | 1 | 0x06 |
| 0x0b | 1 | 0x00 |
| 0x0c | 20 | device name, NUL padded |
| 0x20 | 1 | 0x01 (subtype) |
| 0x21 | 1 | device kind: 0x01 CDJ, 0x02 mixer, 0x03 rekordbox |
| 0x22 | 2 | 0x0036 (length) |
| 0x24 | 1 | **device number D** |
| 0x25 | 1 | 0x01 CDJ / 0x02 mixer |
| 0x26 | 6 | MAC |
| 0x2c | 4 | IPv4 |
| 0x30 | 1 | peer count (devices seen including self; drops about 10 s after a device leaves) |
| 0x31 | 3 | 0x00 |
| 0x34 | 1 | 0x01 CDJ / 0x02 mixer |
| 0x35 | 1 | 0x00, **must be 0x64 for CDJ-3000 compatibility** |

beat-link 7.3.0 changelog: sending keep-alives "in a way that is compatible
even with CDJ-3000s that are using player numbers 5 and 6 (they no longer
display a warning dialog nor drop off the network)"; dysentery says the wrong
0x35 value makes CDJ-3000s "repeatedly kick themselves off the network". The
hello packet is one byte longer in CDJ-3000-compatible mode and byte 0x15
changes in all stage packets.

**Self-assignment handshake (what a virtual CDJ does):** 3 hellos, 3
stage-1 claims (N = 1, 2, 3), 3 stage-2 claims with auto flag 0x01 and our
IP and MAC, 3 stage-3 claims with the number we want, then keep-alives
forever. If any device sends a 0x08 defend packet (41 bytes, unicast)
during the claim, abandon that number and try the next. If a mixer sends
0x05 early the stage-3 series may stop. beat-link also waits
`SELF_ASSIGNMENT_WATCH_PERIOD = 4000 ms` after first seeing any device so it
has observed every keep-alive before picking a free number, and it
**defends** its own number with 0x08 if someone claims it later. Players
time out peers after 10 s without a keep-alive.

**Mixer-assigned numbers:** when a CDJ is plugged into a DJM's per-channel
LINK port, the mixer answers the stage-1 claim with 0x01, the player sends
stage-2 with D = 0 and byte 0x0b = 1, the mixer answers 0x03 with the
assigned D, the player sends one stage-3, the mixer sends 0x05. Irrelevant
to Shunt except that the XDJ-XZ "assigns device numbers regardless of
conflicts" to things on its laptop port.

**Safe device numbers:**

- 1 to 4: real player numbers. Required to (a) use the dbserver from
  pre-3000 players, (b) become tempo master or send status, (c) use fader
  start or load commands. Costs one physical player slot.
- 5 to 6: valid only on CDJ-3000 and XDJ-AZ. Pre-3000 gear does not
  understand them. (dj-midi-sender's README claim that 5 to 6 belong to
  mixers is wrong; mixers use 0x21.)
- 7 to 15: beat-link's default self-assignment start (tries 7 upward when
  not using a standard number). Harmless to real players. **Recommended for
  Shunt's Follow mode.** CDJ-3000 dbserver accepts queries only from numbers
  6 or lower; older players only from 1 to 4, so a 7+ device cannot use
  dbserver against real players.
- 0x21 (33): DJM mixers.
- rekordbox: beat-link's `VirtualRekordbox` self-assigns in 0x13 to 0x27
  (19 to 39); kyleawayan's working rekordbox-lighting keep-alive uses 0x17
  (23) with kind byte 0x03. The "17 / 41" numbers sometimes quoted were not
  confirmed in any reachable source.
- Opus Quad decks announce as 9, 10, 11, 12 (beat-link maps deck = n & 7).

**Pitfalls:** claiming a number in use triggers a defend packet and a
"player number conflict" dialog on real players; two Pro DJ Link apps on
one host conflict for the sockets; pre-3000 players can only serve dbserver
to about 3 clients; using 1 to 4 with four physical CDJs forces one CDJ off
the network.

## 2. Beat packets (port 50001, type 0x28, 0x60 = 96 bytes)

Sent **once per beat, at beat onset**, by every *playing* player with a
rekordbox-analysed track (also by the mixer as a free-running metronome at
the master tempo; its Bb is meaningless). Stopped players send none.

| Offset | Len | Field | Notes |
|---|---|---|---|
| 0x00 | 10 | magic | |
| 0x0a | 1 | 0x28 | |
| 0x0b | 20 | device name | |
| 0x20 | 1 | subtype 0x00 | |
| 0x21 | 1 | D | 1 to 6 players, 0x21 mixer |
| 0x22 | 2 | len_r = 0x003c | |
| 0x24 | 4 | nextBeat | ms until next beat, **at 0 percent pitch** |
| 0x28 | 4 | 2ndBeat | ms to the beat after next |
| 0x2c | 4 | nextBar | ms to next downbeat |
| 0x30 | 4 | 4thBeat | |
| 0x34 | 4 | 2ndBar | |
| 0x38 | 4 | 8thBeat | |
| 0x3c | 24 | 0xff padding | |
| 0x54 | 4 | pitch | 0x00100000 = +0 percent, 0x00200000 = +100 percent, 0 = -100 percent (beat-link reads 3 bytes at 0x55) |
| 0x58 | 2 | 0x0000 | |
| 0x5a | 2 | BPM x 100 | track grid BPM, 12050 = 120.50 |
| 0x5c | 1 | Bb | beat within bar 1 to 4 |
| 0x5d | 2 | 0x0000 | |
| 0x5f | 1 | D again | |

Maths (beat-link `Util`): `NEUTRAL_PITCH = 1048576`;
`pitchPercent = (pitch - 1048576) / 10485.76`; `multiplier = pitch / 1048576.0`;
`effectiveBPM = (bpm100 / 100.0) * multiplier`. dysentery's single-expression
form: `effectiveBPM = (bpm100 * pitch24) / 6400000` with `pitch24` the 3-byte
value at 0x55 to 0x57. The six interval fields are at nominal speed: divide
by the multiplier for real time. Beat packets carry no beat number and no
playback position; that comes from status (0xa0) or precise-position
packets. Bb is "accurate for players when the track was properly configured
within rekordbox (and if the music follows a standard 4/4)".

**Precise position (type 0x0b, CDJ-3000 only, about every 30 ms while a
track is loaded, playing or not; also from Opus Quad when poked with a CDJ
keep-alive):**

| Offset | Len | Field |
|---|---|---|
| 0x0a | 1 | 0x0b |
| 0x0b | 20 | name |
| 0x21 | 1 | D |
| 0x22 | 2 | len_r |
| 0x24 | 4 | track length, seconds (rounded down) |
| 0x28 | 4 | playhead, ms, signed |
| 0x2c | 4 | pitch, signed, percent x 64 (326 = +3.26 percent); a different encoding from every other packet |
| 0x30 | 8 | 0 |
| 0x38 | 4 | BPM x 10 (1202 = 120.2); 0xffffffff unknown |

(dysentery lists these relative to the name field; the absolute offsets
assume the standard name block. Verify against a capture.) beat-link's
minimum accepted length is 60 bytes. BLT 8 added a switch to disable them
because they "currently seem to exhibit too much jitter for use when
synchronizing with audio sources over Ableton Link".

**Other port 50001 packets:**

- 0x03 channels on air: 0x2d bytes (4 channels, flags at 0x24 to 0x27) or
  0x35 bytes with subtype 0x03 (6 channels, channels 5 and 6 at 0x2d to
  0x2e). 0x00 off air, 0x01 on air. Sent by DJM and XDJ-XZ, broadcast.
  Players set their own on-air flag (status F bit 3) from it.
- 0x02 fader start: 0x28 bytes, per-channel command at 0x23 + ch: 0x00
  start, 0x01 stop, 0x02 no change. Not supported by XDJ-XZ or CDJ-3000.
- 0x26 master-handoff request: 0x28 bytes, requester's number at 0x21,
  unicast to the current master's 50001.
- 0x27 master-handoff response: 0x2c bytes, responder's number at 0x21,
  yield flag at 0x2b (1 = accepted).
- 0x2a sync control: 0x2c bytes, command byte at 0x2b: 0x10 sync on, 0x20
  sync off, 0x01 "become master" (0x02 variant also noted).

## 3. CDJ status packets (port 50002, type 0x0a)

Unicast to every announced device, nominally every 200 ms (beat-link's own
sender defaults to 200 ms, allowed 20 to 2000, and avoids sending within a
window around its own beat packets). Newer players burst faster during jog
or search. Lengths: pre-nexus 0xd0, nexus 0xd4, nxs2 0x11c or 0x124,
XDJ-1000 0x11b, XDJ-AZ 0x124, CDJ-3000 0x200. beat-link requires at least
0xcc (204) bytes and at least 0x1ca for loop fields.

Key fields (absolute offsets; name at 0x0b, subtype 0x20, D 0x21, len_r
0x22):

| Offset | Field | Values |
|---|---|---|
| 0x24 | D again | |
| 0x27 | A activity | 0 idle, 1 playing, searching or loading |
| 0x28 | Dr | player the track came from |
| 0x29 | Sr | slot: 0 none, 1 CD, 2 SD, 3 USB, 4 rekordbox collection, 6 streaming direct play, 7 USB-2 (XDJ-AZ), 9 Beatport LINK |
| 0x2a | Tr | track type: 0 none, 1 rekordbox, 2 unanalysed, 5 CD audio, 6 streaming |
| 0x2c | 4 bytes | **rekordbox id** of loaded track |
| 0x32 | 2 | track number in playlist or menu |
| 0x7b | P1 | 0 no track, 2 loading, 3 playing, 4 looping, 5 paused, 6 cued, 7 cue play, 8 cue scratch, 9 searching, 0x0e CD spun down, 0x11 ended, 0x12 emergency loop |
| 0x7c | 4 | firmware ASCII |
| 0x84 | 4 | Sync_n (master priority counter) |
| **0x89** | **F** | bit 6 0x40 Play, bit 5 0x20 Master, bit 4 0x10 Sync, bit 3 0x08 On air, bit 1 0x02 BPM sync (degraded) |
| 0x8b | P2 | 0x7a moving, 0x7e stopped (nexus; varies by model) |
| 0x8c | 4 | Pitch_1 (current pitch including sync; **use this for tempo following**) |
| 0x90 | 2 | Mv: 0x7fff no track, 0x8000 rekordbox, 0x0000 non-rekordbox |
| **0x92** | **2** | **BPM x 100; 0xffff when unknown** |
| 0x94 / 0x96 | | slip master validity / slip BPM |
| 0x98 | 4 | Pitch_2 (fader position only, frozen while paused; do not use) |
| 0x9d | P3 | 0 no track, 1 paused or reverse, 9 forward vinyl, 0x0b slipping, 0x0d forward CDJ |
| **0x9e** | **Mm** | 0 not master, 1 master (rekordbox track), 2 master (non-rekordbox) |
| **0x9f** | **Mh** | device number the master is handing off to; 0xff none |
| **0xa0** | **4** | **beat number** (count from track start per grid; 0xffffffff unknown) |
| 0xa4 | 2 | cue countdown in beats; 0x01ff = none or over 64 bars |
| 0xa6 | Bb | beat within bar 1 to 4 |
| 0xb3 | | 0xff when the beat grid changed |
| 0xc0 | 4 | Pitch_3 (pitch actually in effect) |
| 0xc4 | 4 | Pitch_4 |
| 0xc8 | 4 | packet counter (may be fixed on CDJ-3000) |
| 0xcc | nx | 0x0f nexus, 0x1f XDJ-XZ and CDJ-3000, 0x05 older |
| 0x116 / 0x11a | | time steps in bar / position within bar (nxs2+) |
| 0x158 | Mt | master tempo (key lock) on or off |
| 0x15c | 3 | key (CDJ-3000) |
| 0x1b6 / 0x1be / 0x1c8 | | loop start and end (ms x 65536 / 1000) and loop beat count (CDJ-3000) |

Model quirks: pre-nexus players lack the F byte (beat-link infers play state
from P1); XDJ-XZ embodies two decks and a mixer on one IP and only exposes
its USB slots as player 1; XDJ-AZ status is 292 bytes and beat-link treats it
as "in Pro DJ Link mode" only if a beat packet was seen within 1 s; CDJ-3000
adds 0x0b packets, loop and key fields and accepts numbers 1 to 6; Opus Quad
"does not attempt to truly implement Pro DJ Link": it sends keep-alives with
decks 9 to 12 but only emits status after it receives rekordbox-lighting
style announcements (kind 0x03 keep-alive) plus a lighting request packet on
50002, sends **no beat packets** (phase known only to ±200 ms), cannot serve
metadata, and omits USB-slot and loop info.

**Mixer status (type 0x29 on 50002, 0x38 = 56 bytes):** F at 0x27 (0xf0
master / 0xd0 not, same bit masks), pitch at 0x28 (always +0 percent), **BPM
x 100 at 0x2e**, Mh at 0x36, Bb at 0x37. The BPM is only valid while a
rekordbox-analysed source is playing; a DJM doing its own beat detection
displays a BPM but does not send it.

## 4. Mixer behaviour

- On air: the DJM broadcasts 0x03 on 50001 whenever channel fader,
  crossfader or on/off state changes, and periodically. DJM-V10 (6 channels)
  sends the 0x35-byte variant. DJM-A9 is 4-channel and behaves like the
  900NXS2. XDJ-XZ's embedded mixer sends on air but no fader start.
- Mixer status (0x29) gives the mixer's own metronome tempo and phase and
  whether the mixer is master (F bit 5). The mixer can be tempo master (no
  player playing); it then emits beat packets at the last master BPM.
- For Shunt the on-air packet is the primary source for "which channel is
  live"; the per-player F bit 3 is a derived copy.

## 5. Tempo-master semantics and position estimation

- **Who is master:** the device whose status has F bit 5 set (CDJ: also Mm
  not 0). beat-link keeps the latest such update and treats beats as master
  beats only from that device. Tempo-change listeners fire only when the
  change exceeds 0.0001 BPM.
- **Handoff:** requester sends 0x26 to the master's 50001; master answers
  0x27; the outgoing master keeps F.master set but puts the new number in
  Mh; once the new master asserts F.master, the old master clears to Mh =
  0xff and bumps its Sync_n above everyone else. Unsolicited handoff: if the
  master stops while another synced player is playing, it sets Mh to that
  player. If no master exists a device can simply assert master. To become
  master beat-link requires a number 1 to 4 ("Can only send status when
  using a standard player number"); dj-midi-sender idles on 7 and
  temporarily claims a 1 to 4 slot only while acting as master.
- **Position between beats (TimeFinder):** `moved = round(pitchMultiplier *
  elapsedMs)`; `pos = lastPos ± moved`. Beat packets re-anchor using the
  beat number from the last status packet plus the grid's time for that
  beat, after which status packets from that player are ignored until the
  next beat. With CDJ-3000 0x0b packets, status packets are ignored
  entirely. 50 ms slack before listeners are notified of drift; zero when
  stopped. Beat grid comes from the dbserver (0x2204 request, 16-byte
  entries: Bb u16 LE, BPM x 100 u16 LE, time ms u32 LE, 8 unknown) or from
  ANLZ .DAT files via NFS.
- **Shunt does not need absolute track position**: beat phase =
  `(now - t_lastBeat) * effectiveBPM / 60000`, bar phase from Bb, tempo from
  Pitch_1 x BPM (status, 5 Hz) corrected by each beat. dj-midi-sender found
  that using status-packet pitch gives about 200 ms response to pitch-fader
  moves versus about 500 ms from beats alone.
- **When no beats arrive:** paused or stopped master, keep the last tempo
  and flag free-running; the DJM's metronome beats (D = 0x21) continue at
  the last BPM with meaningless Bb. No track or unanalysed: status BPM
  0xffff, Tr = 2; CDJ-3000 still sends 0x0b with BPM x 10 if it has a
  detected tempo. rekordbox appears as another device (kind 0x03).

## 6. Metadata for tracklist logging

**Path A, dbserver (TCP):** send `00 00 00 0f "RemoteDBServer" 00` (19
bytes) to the player's TCP 12523, get a 2-byte port (usually 1051).
Messages: magic `87 23 49 ae`, transaction id, u16 type, u8 argc, 12-byte
arg-type blob (0x0f/0x10/0x11 = u8/u16/u32, 0x14 blob, 0x26 UTF-16BE
string), args. Greeting: type 0x0000, transaction 0xfffffffe, one u32 =
*our* player number; reply 0x4000. Track metadata: type 0x2002 with a DMST
u32 (requesting device, menu 0x01, slot, track type) plus rekordbox id,
then a render request. Beat grid 0x2204 (menu 8). Constraints: requesting
number 1 to 4 (1 to 6 for CDJ-3000 and XDJ-AZ), must differ from the
queried player, about 3 concurrent clients per pre-3000 player, 10 s socket
timeout. beat-link "borrows" a real player's number when the virtual CDJ is
above 4, which works but is a hack.

**Path B, NFSv2/UDP (Crate Digger):** mount `/B/` (SD) or `/C/` (USB),
fetch `PIONEER/rekordbox/export.pdb` and `PIONEER/USBANLZ/.../ANLZ*.DAT/.EXT`;
rekordbox 7 exports "Device Library Plus" `exportLibrary.db` (SQLCipher,
needs a key). Works from any device number; costs a multi-MB download per
USB insertion; NFS auth is a hard-coded header value (the subject of the
2025 security advisory). Opus Quad does not serve NFS.

**Minimum for Shunt's tracklist:** from status alone you get player, slot,
track type, rekordbox id, track number, load timestamp and first on-air
timestamp. Title and artist require Path A or B. A pragmatic v1: log ids and
timestamps always; resolve title and artist via dbserver only when Shunt is
configured with a real number 1 to 6; else offer NFS as an explicit opt-in;
else match ids against the user's own rekordbox export afterwards.

## 7. Timing and precision

- Beat packets are emitted at beat onset by the player's DSP; BLT's latency
  control defaults to **1 ms**, negative allowed. On a wired switch the beat
  packet lands within about 1 ms of the audible beat, plus the player's own
  audio-output latency, which BLT ignores.
- No published wired-vs-Wi-Fi jitter measurement was found. DjManager's
  XDJ-AZ implementation reports a playhead model (base position, base time,
  speed, 20 percent error correction per observation, hard snap over 400 ms)
  "measured ±1.3 ms on real hardware" (wired). Precise-position packets
  (30 Hz) are jittery; beats are smoother. Over Wi-Fi expect tens of ms and
  loss; Pioneer only supports Wi-Fi Pro DJ Link on CDJ-3000X and
  XDJ-AZ-class hardware.
- Link bridging in beat-carabiner: on each master beat compute the Link beat
  number from a running counter (bar-aligned using Bb) and call
  `force-beat-at-time <beat> <timestamp_us - latency>`; set bpm when tempo
  changes beyond epsilon.
- MIDI clock from this data: dj-midi-sender uses a hardware-timer 24 ppqn
  clock with a dual-source PLL (status pitch for tempo, beats for phase).
  That is the right shape for Shunt.

## 8. Network requirements

- One L2 segment: players, mixer and Shunt on the same switch and subnet.
  Players default to auto IP; with no DHCP they self-assign 169.254.0.0/16
  (RFC 3927) and Shunt must do the same on that interface.
- Unmanaged gigabit switch; no Wi-Fi for the beat path. One Pro DJ Link
  stack per host.
- Official alternatives: PRO DJ LINK Bridge and ShowKontrol speak **TCNet**:
  UDP 60000 (opt-in and status, 1 Hz), 60001 (time, 60 Hz), 60002, unicast
  65023+; 8 layers with state, time, timecode, beat marker, BPM, pitch,
  artist and title, track id, on air. Third-party receivers need a licence;
  node-tcnet and TCNet-dotnet exist but were "tested only against the
  Bridge". A possible "official" input mode for Shunt later, at the cost of
  another PC in the chain.

## 9. Libraries and licences

| Project | Language | Licence | Notes |
|---|---|---|---|
| dysentery | Clojure, AsciiDoc | EPL-1.0 (code) | canonical byte-level analysis |
| beat-link | Java | EPL-2.0 | reference implementation |
| crate-digger | Java, Kaitai | EPL-2.0, MPL-2.0 / LGPL-3 secondary | NFS and PDB parsing; Kaitai `.ksy` specs reusable in C++ |
| beat-carabiner, Open Beat Control | Clojure | EPL-2.0 | Link bridge logic |
| Carabiner | C++ | GPL-2.0 (Link plus Mongoose) | do not link; use the Link SDK directly |
| prolink-connect | TypeScript | MIT | good now-playing and on-air heuristics |
| python-prodj-link | Python | Apache-2.0 | MIDI clock sender; "can freeze your players" |
| prolink-cpp (grantHarris) | C++17, POSIX sockets | MIT | discovery, beats, status, follow master, handoff; macOS and Linux only |
| libcdj (teknopaul) | C | not stated | virtual CDJ plus monitors, Linux and Pi |
| usr-ein/prolink | Rust | GPL-3.0 | capture-derived, 647 tests |
| dj-midi-sender | ESP32 C++ | PolyForm Noncommercial | design notes only |
| SuperTimecodeConverter | C++17, JUCE | MIT | virtual CDJ, 0x0b to LTC/MTC/Art-Net/TCNet, Link; closest prior art |
| pro-dj-link-monitor | Node, Electron | MIT plus one EPL-2 file from beat-link | |
| opus-quad analysis (kyleawayan) | Python | MIT | rekordbox-lighting trick |

**Clean-room status:** the packet layouts are published facts;
reimplementing them in C++ from the documentation is not a derivative work
of beat-link's EPL code. Do not copy byte-array templates verbatim with
comments from beat-link. MIT C++ references exist (prolink-cpp,
SuperTimecodeConverter) if code is borrowed. The real licence trap is
**Ableton Link (GPLv2 or paid)**. It is all reverse-engineered with no spec
from AlphaTheta; firmware updates can change behaviour (the CDJ-3000
keep-alive byte 0x35 was exactly such a break).

## Recommended network stack for Shunt

Threads, with lock-free SPSC queues to the clock and output threads:

1. **AnnounceThread**: one UDP socket on 0.0.0.0:50000 with SO_BROADCAST
   and SO_REUSEADDR; parses 0x06 keep-alives into a device table (expire at
   10 s); runs the claim state machine; sends keep-alive every 1500 ms on
   the subnet broadcast with byte 0x35 = 0x64 and CDJ-3000-style hello;
   sends 0x08 defend if someone claims our number.
2. **BeatThread**: socket 50001, highest priority; timestamps each datagram
   with a monotonic clock at `recv()` (SO_TIMESTAMPNS on Linux); dispatches
   0x28, 0x0b, 0x03, 0x26/0x27/0x2a.
3. **StatusThread**: socket 50002; parses 0x0a (len at least 0xcc) and
   0x29; publishes per-device `PlayerState` snapshots.
4. **ClockEngine**: the master-follower PLL: tempo from Pitch_1 x BPM in the
   master's status, phase corrected on each master beat packet (minus
   latency, bar-aligned from Bb); outputs Link, 24 ppqn MIDI clock, OSC.
   Free-runs at the last tempo when beats stop and marks confidence low.
5. **MetadataThread** (optional): dbserver TCP client or NFS fetcher; never
   on the hot path.
6. **Logger**: tracklist events (load on rekordbox id change, on air on F
   bit 3 rising or 0x03, unload) with resolved title and artist when
   available.

Master-selection state machine: `NoMaster -> Master(n)` on the first status
with F.master; on Mh not 0xff from master n, pre-arm `Handoff(n -> m)`;
switch to `Master(m)` when m asserts F.master; fall back to the mixer's 0x29
BPM and beat if no player is master. Device-number state machine: `Idle ->
Watching(4 s) -> Claim(hello x3 -> s1 x3 -> s2 x3 -> s3 x3) -> Active` with
`Defended -> next number`. Modes: *Follow* (number 7+, never sends status)
and *Lead* (number 1 to 6, sends 0x0a status at 5 Hz plus 0x28 beats,
performs handoff).

Sockets: bind to the specific interface, compute broadcast from its netmask,
support 169.254/16 self-assignment when no DHCP lease. One process per host.

## Risks

- Reverse-engineered protocol; new firmware (CDJ-3000X, XDJ-AZ, DJM-A9) may
  break assumptions; ship a packet-capture and diagnostic mode.
- Opus Quad: no beats, ±200 ms phase; needs rekordbox-lighting impersonation
  and metadata archives; treat as tempo only.
- Device-number collisions and the 1 to 4 slot cost; CDJ-3000s dropping off
  the network if keep-alive bytes are wrong.
- Metadata needs a real player number or NFS; both fragile and slow; XDJ-RX
  series cannot participate at all.
- Timing: beat packets are per beat only on pre-3000 gear; 0x0b packets are
  jittery; Wi-Fi unusable; no published jitter data, so Shunt must measure
  its own and expose a latency offset per output.
- Licences: Link SDK GPLv2 or commercial; avoid copying EPL and GPL code.
- Mixer BPM invalid for non-rekordbox sources; status BPM 0xffff for
  unanalysed tracks; "unknown tempo" must be handled gracefully.

## Sources

- dysentery analysis sources: https://github.com/Deep-Symmetry/dysentery (doc/modules/ROOT/pages: packets.adoc, startup.adoc, beats.adoc, vcdj.adoc, sync.adoc, mixer_integration.adoc, track_metadata.adoc); rendered at https://djl-analysis.deepsymmetry.org/
- beat-link sources: https://github.com/Deep-Symmetry/beat-link (VirtualCdj.java, BeatFinder.java, Beat.java, CdjStatus.java, MixerStatus.java, Util.java, DeviceFinder.java, DeviceAnnouncement.java, VirtualRekordbox.java, data/TimeFinder.java, data/OpusProvider.java, dbserver/ConnectionManager.java, README.md, CHANGELOG.md)
- beat-link-trigger guide sources: https://github.com/Deep-Symmetry/beat-link-trigger (Link.adoc, OpusQuad.adoc, Players.adoc)
- Crate Digger: https://github.com/Deep-Symmetry/crate-digger
- beat-carabiner: https://github.com/Deep-Symmetry/beat-carabiner ; Carabiner: https://github.com/Deep-Symmetry/carabiner
- prolink-connect: https://github.com/EvanPurkhiser/prolink-connect
- python-prodj-link: https://github.com/flesniak/python-prodj-link
- Opus Quad analysis: https://github.com/kyleawayan/opus-quad-pro-dj-link-analysis
- prolink-cpp: https://github.com/grantHarris/prolink-cpp ; libcdj: https://github.com/teknopaul/libcdj ; Rust prolink: https://github.com/usr-ein/prolink ; dj-midi-sender: https://github.com/Calvin-Zikakis/dj-midi-sender
- SuperTimecodeConverter: https://github.com/fiverecords/SuperTimecodeConverter ; PRO DJ LINK Monitor: https://github.com/PROcrush/pro-dj-link-monitor
- DjManager XDJ-AZ model: https://github.com/Radexito/DjManager/issues/604
- TCNet: https://github.com/chdxD1/node-tcnet ; https://github.com/evanminton/TCNet-dotnet ; https://docs.rs/tcnet/latest/tcnet/
- Pioneer PRO DJ LINK Bridge (via search): https://www.pioneerdj.com/en/product/software/pro-dj-link-bridge/software/overview/ ; CDJ-3000 firmware 1.10: https://www.pioneerdj.com/en/news/2021/cdj-3000-firmware-ver-110-update/
- Networking guidance (via search): https://djtechtools.com/2018/07/31/pro-dj-link-how-to-set-up-pioneer-dj-setups-properly/ ; https://community.pioneerdj.com/hc/en-us/community/posts/34021336991385-Pro-Link-DHCP ; https://blt-guide.deepsymmetry.org/beat-link-trigger/8.0.0/Link.html
