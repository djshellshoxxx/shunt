# Research 03: Timing pipeline, appliance, desktop, integrations, logging, testing

Status: research notes, 7 October 2026. Inputs to the engineering specs.
Ableton Link API and MIDI clock generation basics are covered in the
Pacemaker repo (`docs/research/02-clock-output-mechanisms.md`) and not
repeated. Several primary sources were blocked by the research proxy; the
beat-link, beat-carabiner and beat-link-trigger source files on GitHub were
read directly. Items from memory are flagged **[unverified]**.

---

## 1. Timing pipeline: beat packets to a smooth clock

### 1.1 What the input is

From `Beat.java` in beat-link (verified): one UDP packet per beat on port
50001 from each player with pitch at 0x55 (3 bytes, 0x100000 = +0 percent),
BPM x 100 at 0x5a (2 bytes), beat-within-bar at 0x5c (1 to 4; "meaningless
from mixer devices"; only correct if the rekordbox grid is right and the
track is 4/4), and ms offsets *at +0 percent pitch* to the next beat (0x24),
2nd beat (0x28), next bar (0x2c), 4th beat (0x30), 2nd bar (0x34), 8th beat
(0x38), 0xffffffff when the track ends first. Effective tempo =
`bpm * pitchMultiplier / 100`.

Status packets (port 50002, about every 200 ms) carry play state, master
flag, beat number in track, and on CDJ-3000 precise-position packets. Beat
packets stop when a deck is paused; the status packet is the only thing
that distinguishes "paused" from "network died".

### 1.2 What the reference implementations do

**beat-link `TimeFinder`** (verified): keeps a `TrackPositionUpdate
{timestamp, milliseconds, beatNumber, pitch, playing, reverse, fromBeat,
precise}` per player; `getTimeFor()` dead-reckons `moved = round(pitch *
elapsedMillis)` from the last update. On a beat packet it only increments
the beat number if the previous estimate is at least 1/5 of a beat into the
current beat (`farEnough = 6_000_000 / bpm / 5`), which rejects duplicate or
early packets. `slack` = **50 ms**: if interpolation and the new packet
disagree by more while playing, listeners are told a "jump" happened. Pitch
change tolerance 0.001 (beat-sourced), 0.000001 (status-sourced). Loops: beat
number capped at the grid's beat count; cue jumps: on stopped to playing it
snaps to a cue within 2 beats.

**beat-carabiner** (verified from `core.clj`): modes off, passive (Link
always follows DJ Link), manual, full (bidirectional; requires device number
1 to 4). BPM changes below **0.00001 BPM** are ignored; skew tolerance
default **0.0166 beat (1/60 beat, about 7.8 ms at 128 BPM)**: if the Link
phase is off by more it issues `force-beat-at-time` (a hard jump); otherwise
`beat-at-time` (soft). Bar alignment maps `getBeatWithinBar()` to quantum 4
via `bar-skew = (beatNumber - 1) - (rawBeat mod 4)`, wrapped to [-1, 2]. In
full mode a daemon thread re-probes phase periodically and only corrects if
the correction stays within the same beat or moves more than 1/5 beat.

**Beat Link Trigger Carabiner window** (verified from `Link.adoc`): modes
Off, Triggers, Passive, Full; an "Align bars" checkbox; a **latency** field
defaulting to **1 ms** = delay between the audible beat and the packet
arriving. Quoted caveat: when Pioneer gear follows Link it "will only sync
to individual beats, and won't attempt to line up downbeats".

So the mature reference does **no filtering**: it dead-reckons from the
last packet at the reported pitch and hard-jumps Link whenever error exceeds
1/60 beat. Fine on a dedicated switch, twitchy on anything worse.

### 1.3 Network jitter to expect

- Dedicated gigabit switch: store-and-forward of a 100-byte frame is about
  1 µs; measured Ethernet jitter is tens of µs. CDJ firmware scheduling
  jitter dominates; the BLT 1 ms latency default and 50 ms slack are the
  practical envelope. Wired floor: **±0.1 to 1 ms**.
- Consumer router with other traffic: sub-ms typical, occasional multi-ms
  spikes; broadcast storms can add tens of ms.
- Wi-Fi: lab reports average 77 to 150 µs jitter but 2.2 to 2.7 ms maxima in
  a clean lab; in a club with 2.4 GHz contention expect **5 to 30 ms
  typical, 100 to 300 ms spikes** on power-save or roaming. Shunt must use
  Wi-Fi for the config UI only; the Pro DJ Link NIC must be wired; the
  filter must survive 50 to 100 ms outliers anyway.

### 1.4 Events the filter must survive

| Event | Signature | Required response |
|---|---|---|
| Master handoff (two decks, different pitch) | master flag moves in status packets; new master's beats have different tempo and arbitrary phase | one hard re-phase, tempo slews; keep a per-player filter warm |
| Jog, nudge, pitch bend | beat spacing changes abruptly for a few beats; pitch field unchanged (nudge) or changed (slider) | widen filter bandwidth temporarily; do not push Link tempo per beat; rate-limit tempo pushes and quantise to 0.01 BPM |
| Loop or hot cue | beat number in status jumps; beat-within-bar may jump; spacing still regular | beat phase usually continuous for quantised loops; re-derive bar phase from beat-within-bar; do not touch tempo; un-quantised cues land early: one forced jump |
| Pause | beat packets stop; status play flag = stopped within 200 ms | freeze the clock; configurable: keep MIDI clock running (lighting wants this) or send Stop; Link stop-state if start/stop sync is on; on resume re-phase from the first two beats |
| Packet loss | gap of exactly N periods | dead-reckon; count missed beats from the model; do not declare paused until status says so or over 1 s elapsed |
| Duplicate or reordered | arrival under 1/5 beat after the previous | drop (TimeFinder rule) |

### 1.5 Mapping beat-in-bar to Link quantum 4, and wrong grids

Link phase (quantum 4) is in [0, 4). Set the Link beat at the packet's
predicted beat time to `(beatWithinBar - 1) + 4k`. When a bar marker is
wrong by one or two beats (common: tracks gridded from beat 3, 2-beat drum
intros), the DJ hears nothing wrong but every quantised Live clip launches
on the wrong downbeat. Provide: a per-session **bar offset** control (0 to
3, cycle button, exposed via OSC and MIDI), a "beat-only" mode that aligns
quantum 1 (what Pioneer does when following Link), and a memory keyed by
rekordbox track ID plus source for offsets applied before. When the master
is a DJM (only when no CDJ is master), hold the last known bar phase; DJM
status has no meaningful beat-within-bar.

### 1.6 Avoiding dragging Link and MIDI clock

- Deadband: apply new tempo only if the change exceeds 0.02 BPM
  (beat-carabiner's 0.00001 is effectively every packet; too eager for a
  filtered design).
- Phase threshold: correct phase only if the error exceeds about 8 ms (1/60
  beat) for two consecutive beats. Correct softly via `requestBeatAtTime`;
  force only on discontinuity events.
- Rate limit: at most one tempo commit per second, one forced jump per 4
  beats.
- MIDI clock: never re-time individual ticks from packets; run the 24 ppqn
  generator from the filtered model; absorb phase error by stretching or
  shrinking tick intervals by at most 2 percent over a bar unless the error
  exceeds half a beat (then skip or insert ticks once).

## 2. Headless Raspberry Pi appliance

### 2.1 JUCE without a display

JUCE 6+ moved X11 into `juce_gui_basics`; a console app using only
`juce_core`, `juce_events`, `juce_audio_basics`, `juce_audio_devices` and
`juce_osc` links without libX11 on Linux. The persistent gotcha: the
MessageManager is not running in a console app, so `juce::Timer`,
`AsyncUpdater`, OSC receivers and MIDI callbacks that post to the message
thread do nothing until `MessageManager::getInstance()->runDispatchLoop()`
is called (or `runDispatchLoopUntil` in your own loop with a SIGTERM
handler). Build with `juce_add_console_app`, `JUCE_USE_CURL=0`,
`JUCE_WEB_BROWSER=0`. Dependencies on Pi OS Lite: `libasound2-dev`.
Cross-compile with the aarch64 toolchain in CI or build natively on a Pi 4
(3 to 4 minutes for this module set).

### 2.2 System image

- Base: Raspberry Pi OS Lite 64-bit (Bookworm). Buildroot or Yocto later.
- systemd unit: `Type=simple`, `Restart=always`, `RestartSec=2`,
  `WatchdogSec=10` (write `WATCHDOG=1` to `$NOTIFY_SOCKET` yourself to avoid
  libsystemd), `Nice=-10`, `LimitRTPRIO=95` plus `CAP_SYS_NICE` so the clock
  thread can use `SCHED_FIFO`. Hardware watchdog `RuntimeWatchdogSec=15`.
- Power-cut safety: `raspi-config nonint enable_overlayfs` and
  `enable_bootro` give a read-only lower rootfs with a tmpfs upper layer.
  Keep a third partition `/data` (ext4, `data=journal`, rw, `nofail`) for
  config, tracklist logs and settings; `fsync()` each tracklist row.
  `journald Storage=volatile`. Updates flip the overlay off, apply, reboot.

### 2.3 Networking

- eth0 to the DJ switch. CDJs with no DHCP self-assign 169.254.x.y derived
  from the last two MAC bytes (MAC ...55:9A becomes 169.254.85.154).
  NetworkManager: `ipv4.method=auto` with `ipv4.link-local=fallback` so the
  Pi lands in 169.254/16 after DHCP timeout (`ipv4.dhcp-timeout=10`, else
  the fallback takes 45 s). RFC 3927 ARP probing avoids conflicts. Only one
  Pro DJ Link application per adapter: rekordbox and Shunt on the same NIC is
  impossible; passive sniffing is the workaround Now Playing uses.
- wlan0 as a NetworkManager hotspot `Shunt-xxxx` (WPA2, 10.42.0.1/24) by
  default, switchable to client mode. Never route between wlan0 and eth0
  (`ip_forward=0`, nftables drop forward). Link multicast (224.76.78.75:20808)
  should bind to the Link interface, usually the house LAN, not the CDJ
  VLAN; make it selectable.
- mDNS: avahi advertising `_http._tcp` 80 and `_shunt._tcp` for
  `http://shunt.local`; restrict avahi to wlan0.

### 2.4 Embedded web UI

| Option | Licence | WebSocket | Notes |
|---|---|---|---|
| civetweb | MIT | yes | multi-threaded, TLS optional, C++ wrapper, about 200 KB. **Recommended.** |
| cpp-httplib | MIT, header-only | no (SSE via chunked provider) | simplest; SSE at 10 Hz is enough for a status page |
| Mongoose | GPLv2 or commercial | yes | no for closed source without a licence |
| JUCE StreamingSocket | JUCE | DIY | not worth writing HTTP and WS parsing |

Serve one embedded HTML file (`juce::BinaryData`), JSON REST for config,
WebSocket or SSE push of `{bpm, phase, master, players[], link_peers}` at
10 Hz.

### 2.5 Status display and GPIO

- SSD1306 128x64 I2C (about $4): `/dev/i2c-1` via `ioctl(I2C_SLAVE)` and
  raw page writes; shows BPM, master deck, Link peers, IP.
- WS2812 via `/dev/spidev0.0` at 2.4 to 3.2 MHz bit-stuffing; one or two
  pixels for beat, bar and status.
- Beat LED: libgpiod v2 (`/dev/gpiochip0`; sysfs GPIO is deprecated; Pi 5
  header chip naming differs on older kernels). Fire from the clock thread.
- DIN MIDI out: (a) **Pisound** (€99, Blokas): DIN in/out plus 192 kHz audio,
  which also gives the audio click output; (b) any class-compliant USB MIDI
  interface ($10 to $40); (c) UART: `dtoverlay=midi-uart0` plus
  `dtoverlay=disable-bt` puts PL011 on GPIO14/15 at 31250 baud; the
  mini-UART (ttyS0) is unusable because its clock follows core frequency;
  `midi-uart0` does not work on Pi 5 (RP1 UART; a `midi-uart0-pi5` overlay
  exists in newer firmware **[unverified]**). Hardware: 6N138 optocoupler on
  input, 3.3 V drive with 33 and 10 ohm series resistors on output, about $3
  of parts.
- USB MIDI gadget (Pi appears to the laptop as a USB MIDI device):
  `dtoverlay=dwc2`, `modprobe libcomposite`, configfs gadget with
  `functions/midi.usb0` (`f_midi`; kernel 6.x also has `f_midi2`). 2025
  patches allow the interface string to be customised so it shows as
  "Shunt". The gadget appears locally as an ALSA sequencer client that Shunt
  writes to with `juce::MidiOutput`. Works on Pi Zero / Zero 2 W (micro-USB
  OTG), Pi 4 and Pi 5 (USB-C power port). On Pi 4/5 the laptop then powers
  the Pi, so no wall PSU on that port at the same time. Zero 2 W cannot be
  both gadget and host for a USB-Ethernet dongle.

### 2.6 Hardware choice

| Board | 2026 price | Ethernet | Gadget | Notes |
|---|---|---|---|---|
| Pi Zero 2 W | $15 | none (USB dongle) | yes | no wired NIC plus gadget simultaneously; Link-only use |
| Pi 4 (2 GB) | $35 to $45 | gigabit | yes (USB-C) | **best value for v1**; analog audio; PL011 UART MIDI works |
| Pi 5 (2 GB) | $65 | gigabit | yes | overkill; no analog audio; RP1 changes overlays |
| CM4 Lite plus carrier | $35 plus $25 to $45 | gigabit | yes | best for a product enclosure, eMMC, PoE |

CPU and RAM: this module set idles under 3 percent of one Pi 4 core and 30
to 50 MB RSS; Pi OS Lite idles about 150 MB. No NTP or PTP required for
Link, but enable `systemd-timesyncd` on wlan0 so tracklist logs have wall
time.

BOM (minimum, Pi 4): Pi 4 2 GB $40, 16 GB A1 microSD $8, PSU $9, SSD1306
$4, UART MIDI parts $3 (or USB MIDI interface $15 to $40), case $10 to
$16, cable $3: **about $80 to $110**; with Pisound about $190. Circuit
Happy's Missing Link (Pi Zero W, Link to analog clock, web config UI)
retailed around $200 **[unverified]**, so a $249 to $299 Shunt box is in
range.

## 3. Desktop app specifics

- UDP listen: Pro DJ Link beat (50001) and status (50002) packets are
  broadcasts. `juce::DatagramSocket sock(true)`; `setEnablePortReuse(true)`;
  `bindToPort(50001)` on 0.0.0.0, then filter by source address against the
  chosen interface's subnet. Do **not** bind to the interface IP on Linux or
  macOS: a socket bound to a unicast address does not receive broadcasts
  there (Windows does). Verify binding with `getBoundPort()` and a test
  read (the JUCE forum documents `bindToPort` returning true on failure on
  Windows). Interface enumeration via `juce::IPAddress::getAllInterfaceEntries()`;
  pick the interface whose subnet contains the first CDJ announcement seen
  on 50000, as beat-link does; expose an override.
- Sending (keep-alives on 50000 when joining as a virtual CDJ) needs the
  per-interface broadcast address. Device number 5 to 15 is fine for Link,
  MIDI and OSC; 1 to 4 only for dbserver metadata or becoming master.
- Firewalls: macOS prompts once for a signed and notarised app; Windows
  Defender prompts on first bind, add a rule from the installer with
  `netsh advfirewall`; Linux unprivileged ports need nothing.
- Tray or menu bar: `juce::SystemTrayIconComponent`; macOS `LSUIElement=1`
  hides the Dock icon. Auto-start: macOS `SMAppService` or a LaunchAgent;
  Windows `HKCU\...\Run`; Linux `~/.config/autostart`.
- Logging: `juce::FileLogger::createDateStampedLogger` in the platform log
  folder; a separate optional binary packet trace that doubles as a replay
  input (section 6).

## 4. OSC and lighting integrations

| Target | Accepts | Shunt should send |
|---|---|---|
| Resolume Arena/Avenue 7 | native Link; OSC `/composition/tempocontroller/tempo f` and `/composition/tempocontroller/resync`; TCNet via ShowKontrol | prefer Link; OSC fallback: tempo on change, `resync` on each bar or on bar-offset change |
| TouchDesigner | Link CHOP (tempo, beat, phase, start/stop); OSC In CHOP | Link; raw OSC for those without the CHOP |
| grandMA3 | OSC command line `/gma3/cmd s`; speed masters `Master 3.x` (`Master 3.1 At BPM 128.0`); "Sound in" BPM | `/gma3/cmd "Master 3.1 At BPM <v>"` on tempo change (deadband 0.1 BPM); set Receive Command = Yes |
| ChamSys MagicQ | DJ integration via TCNet (Speedmaster source "Audio BPM"); OSC in; MIDI clock **[unverified]** | OSC via AUTOM mapping, or MIDI clock; TCNet only via ShowKontrol |
| Avolites Titan v13+ | "Pioneer DJ BPM Trigger" via the official Bridge; no public Link | MIDI clock into Titan's MIDI BPM master, or OSC (Titan has OSC in from v9) |
| Daslight / Sunlite | MIDI clock BPM sync only; common workaround is Ableton Lite plus LoopBe | Shunt's MIDI clock replaces that workaround |
| QLab | OSC `/cue/<n>/start` etc.; no tempo concept | fire user-mapped cues on master change, track load, bar 1 |
| OBS | obs-websocket v5 `SetInputSettings`; Browser Source | both; the browser-source overlay is zero-config |
| Onyx (Obsidian) | native Link (DyLOS) | Link |

**TCNet**: created by TC Supply for ShowKontrol; Pioneer partnered in 2017
and "only ShowKontrol or the Bridge app are allowed to connect" in the
certified ecosystem; Avolites, ArKaos, ChamSys, Resolume and disguise
consume it. The wire protocol is UDP multicast documented in a TC Supply
PDF; `node-tcnet` on npm exists. Shunt can technically emit TCNet frames,
but "TCNet" is TC Supply's trademark and the compatibility programme is
theirs; ship it only as an experimental "TCNet-compatible (unofficial)"
output after legal review.

## 5. Tracklist logging

**Metadata without touching the USB**: status packets give rekordbox track
ID, source player, slot, play state, BPM, beat, pitch. Title, artist, album,
genre, key, duration, label and artwork come from the **dbserver** (TCP,
port looked up via 12523), only reliable when Shunt holds a real player
number 1 to 4. **CrateDigger** (beat-link dependency, NFS v2) instead
downloads `export.pdb`, `exportExt.pdb` and ANLZ files from the media,
giving the full rekordbox database regardless of device number. For a C++
port: parse PDB with the Kaitai `rekordbox_pdb.ksy` (compile to C++) and a
minimal NFSv2 client (libnfs is LGPL; a minimal RPC/NFS2 READ is about 600
lines). After the 2025 advisory, NFS reads are opt-in and off by default.

**Formats people ask for** (Now Playing's exporter, Serato history,
1001Tracklists usage):

1. CSV mirroring Serato's history columns: `#, name, artist, album, genre,
   bpm, key, start time, end time, played, deck, rekordbox_id, source`.
2. CUE sheet: `PERFORMER`, `TITLE`, `FILE "mix.wav" WAVE`, then `TRACK nn
   AUDIO / TITLE / PERFORMER / INDEX 01 mm:ss:ff` (75 fps), times relative
   to a user-set "set start" marker.
3. Timestamp text `[hh:mm:ss] Artist - Title` for SoundCloud and Mixcloud;
   1001Tracklists accepts `hh:mm Artist - Title` lines with `ID - ID` for
   unknowns (no API).
4. NDJSON per row, appended with fsync.
5. Rights reporting: PRS for Music, ASCAP OnStage and BMI Live are web
   portals with CSV statements but no DJ-specific import schema; provide a
   generic performance report CSV: `date, venue, title, artist/performer,
   writer(s), publisher, ISRC, duration` with writer and ISRC blank for the
   user to fill.

**Now Playing precedent**: reads Pro DJ Link actively or passively (sniffing
when rekordbox owns the port), pulls metadata from dbserver or NFS, renders
overlays via a local HTTP server consumed by an OBS Browser Source, plus
obs-websocket text updates; history export offers CSV, CUE, text templates
and 1001Tracklists. Copy the "local HTTP overlay plus WebSocket push" model.

**"Played" heuristic**: mark a track played when it was master or on-air for
over 30 s or over 20 percent of its length; record start at the first
on-air moment, not at load.

## 6. Testing without CDJs

- **pcaps**: dysentery ships `doc/assets/powerup.pcapng` and
  `doc/assets/to-virtual.pcapng`. Replay with `tcpreplay` after
  `tcprewrite`, or better: a built-in replayer that reads pcapng with
  libpcap, keeps inter-packet timing, and re-emits UDP payloads on
  50000 to 50002 to the broadcast address. This becomes `--replay
  file.pcapng [--speed 1.0 --jitter-ms 5 --loss 0.02]`. Record captures from
  any rented booth with `tcpdump -i eth0 -w set.pcapng udp portrange
  50000-50002`; add a "record session" button to the web UI; this is also
  the bug-report format.
- **Simulators**: beat-link has no standalone CDJ simulator; BLT's
  Simulator menu is JVM-only; `prolink-connect` implements a virtual CDJ
  that produces announce and status packets **[unverified it emits beats]**.
  Build **ShuntSim**: a small tool emitting a 4-deck network (announce every
  1.5 s on 50000, status every 200 ms on 50002, beats on 50001 at
  `60 / (bpm * pitch)` s) with scripted scenarios: master handoff 128 to 130
  BPM, nudge ±2 percent for 8 beats, hot cue mid-bar, 4-beat loop, pause and
  resume, 3 percent loss, Gaussian jitter sigma 0.5 / 5 / 30 ms with 1
  percent 100 ms outliers.
- **Unit tests for the filter**: feed synthetic (beat index, arrival time)
  series; assert steady-state phase error under 1 ms RMS at sigma 1 ms and
  under 4 ms at sigma 10 ms; tempo settles within ±0.01 BPM in at most 8
  beats after a step; a 100 ms outlier moves the phase estimate under 1 ms;
  a master handoff produces exactly one forced jump; no tempo commit exceeds
  the rate limit. Catch2 under ctest in CI (x86 plus aarch64 via QEMU user
  mode).
- **Hardware in the loop**: cheapest genuine peers second-hand: XDJ-700
  about $600, XDJ-1000MK2 $800 to $1000 **[unverified]**, CDJ-2000
  (non-NXS) often under $500. One player plus a dumb gigabit switch suffices
  (the lone player is always master). Avoid XDJ-RX/RX2.
- **rekordbox as a peer**: rekordbox in Performance mode joins Pro DJ Link
  as a device over a wired adapter, and beat-link accepts it as a tempo and
  beat source, so a laptop running rekordbox plus a second machine running
  Shunt on the same switch is a free test rig. rekordbox grabs the ports on
  its own machine, so Shunt must be on another host or in passive sniff
  mode, and rekordbox needs a real link-up on the adapter.

## 7. Recommended appliance design

- Pi 4 (2 GB) or CM4 Lite on a carrier, Pi OS Lite 64-bit, overlayfs root
  plus rw `/data`, systemd watchdog, hardware watchdog.
- Ports: onboard gigabit Ethernet to the DJ switch (link-local fallback);
  Wi-Fi AP or client for the web UI and Link to the house LAN (selectable);
  USB-C in gadget mode so the laptop sees "Shunt MIDI", and USB-A host for a
  class-compliant interface; DIN out via PL011 UART or Pisound if an audio
  click is required; SSD1306 OLED plus one WS2812 beat pixel; a single
  button (long press = factory reset or Wi-Fi AP).
- Software: one JUCE console process (juce_core, events, audio_basics,
  audio_devices, osc, Link SDK, civetweb); threads: UDP receive (SCHED_FIFO
  80, `SO_TIMESTAMPNS`), estimator, Link/MIDI/OSC emit (SCHED_FIFO 70), web
  and WebSocket, metadata, logger.
- Web UI at `http://shunt.local`: live status, config JSON, record pcap,
  replay, bar offset, update.
- BOM about $80 to $110 (no Pisound) or $190 (with); retail $249 to $299.

## 8. Recommended timing algorithm

1. Timestamping: `SO_TIMESTAMPNS` on Linux, `CLOCK_MONOTONIC` right after
   `recv` elsewhere; subtract a configurable latency (BLT default 1 ms;
   expose 0 to 50 ms).
2. Per-player state machine fed by beats and status: playing, master,
   beatWithinBar, pitch, bpm, beatNumberInTrack. Only the master's beats
   drive the output clock; other filters stay warm for handoffs.
3. Estimator: a 2-state Kalman or alpha-beta filter on (phase, period) in
   beat-index space: predicted `t_n = t_{n-1} + P`; innovation `r = t_n -
   predicted`. Gating: drop packets arriving under 1/5 beat after the
   previous accepted one; treat `|r| > 3 * MAD(r, last 16)` (floor 25 ms) as
   an outlier, ignored for the update but counted; two consecutive outliers
   in the same direction, or a status discontinuity (master change, beat
   number jump, pause to play), **reset** the filter from the last two
   packets and emit one forced phase jump. Gains alpha about 0.3, beta about
   0.05 steady state (about 8-beat settling); alpha 0.7 for 8 beats after a
   pitch-field change or a nudge.
4. Period source: use the packet's effective tempo `bpm * pitch` as a
   low-noise *measurement* of P, and arrival timing only for phase; period
   estimate = 0.9 reported plus 0.1 measured, which removes pitch-bend lag.
5. Bar phase: `linkBeat = (beatWithinBar - 1) + 4k + userBarOffset`; if the
   master is a DJM, hold the previous bar phase; "beat-only" mode uses
   quantum 1.
6. Output coupling to Link: commit tempo only if the change exceeds 0.02 BPM
   and at least 1 s since the last commit; phase via `requestBeatAtTime` when
   the error exceeds 1/60 beat for 2 consecutive beats; `forceBeatAtTime`
   only on reset events. MIDI clock: tick scheduler driven by the filtered
   model with ±2 percent per bar slew; hard resync (drop or insert ticks,
   optional SPP) only after a reset. OSC: tempo on commit, `/beat` and
   `/bar` from predicted times, not packet arrival.
7. Pause: hold tempo; outputs configurable; on resume reset from the first
   two beats.
8. Validation: the synthetic-jitter unit tests, plus a "phase scope" page in
   the web UI plotting `r` over the last 64 beats.

## Sources

- beat-link `Beat.java`: https://raw.githubusercontent.com/Deep-Symmetry/beat-link/main/src/main/java/org/deepsymmetry/beatlink/Beat.java
- beat-link `TimeFinder.java`: https://raw.githubusercontent.com/Deep-Symmetry/beat-link/main/src/main/java/org/deepsymmetry/beatlink/data/TimeFinder.java
- beat-link README: https://github.com/Deep-Symmetry/beat-link/blob/main/README.md
- beat-carabiner `core.clj`: https://raw.githubusercontent.com/Deep-Symmetry/beat-carabiner/main/src/beat_carabiner/core.clj
- Beat Link Trigger Link docs: https://raw.githubusercontent.com/Deep-Symmetry/beat-link-trigger/main/doc/modules/ROOT/pages/Link.adoc ; troubleshooting wiki: https://github.com/Deep-Symmetry/beat-link-trigger/wiki/Troubleshooting-Network-Problems
- dysentery: https://github.com/brunchboy/dysentery
- python-prodj-link: https://github.com/flesniak/python-prodj-link
- Now Playing Pro DJ Link reference: https://nowplayingapp.com/help/reference/integrations/prodjlink/ ; history export: https://nowplayingapp.com/help/guide/history/exporting/
- Open Beat Control: https://github.com/Deep-Symmetry/open-beat-control
- The Missing Link: https://www.synthtopia.com/content/2018/09/12/the-missing-link-brings-ableton-link-to-analog-gear/
- Wi-Fi jitter lab data: https://img-en.fs.com//file/report/fs-ap1167c-latency-test-report-5g.pdf
- JUCE DatagramSocket: https://docs.juce.com/master/classjuce_1_1DatagramSocket.html ; https://forum.juce.com/t/datagramsocket-bindtoport-not-binding-to-right-interface/16423 ; https://forum.juce.com/t/bug-datagramsocket-bindtoport-returns-true-even-if-it-really-fails-at-least-on-windows/20614
- JUCE headless Linux: https://forum.juce.com/t/juce-without-x11/59564 ; https://forum.juce.com/t/run-messagemanager-in-console-application/63683
- Read-only Pi: https://learn.adafruit.com/read-only-raspberry-pi/overview
- Pi UART MIDI: https://discourse.zynthian.org/t/dtoverlay-midi-uart0-on-pi5/9211 ; https://community.blokas.io/t/why-choose-spi-over-uart/2559
- USB MIDI gadget: https://lkml.iu.edu/2512.1/01914.html
- Pisound: https://blokas.io/pisound/
- Pi pricing 2026: https://magazinmehatronika.com/en/the-2026-raspberry-pi-pricing-and-buying-guide/
- Embedded web servers: https://github.com/civetweb/civetweb ; https://www.pistack.xyz/posts/2026-09-04-embedded-c-http-libraries-mongoose-civetweb-libmicrohttpd/
- Resolume OSC: https://github.com/bitfocus/companion-module-resolume-arena ; https://resolume.com/article/63
- grandMA3 OSC and speed masters: https://help.malighting.com/grandMA3/2.3/HTML/remote_inputs_osc.html ; https://help.malighting.com/grandMA3/2.2/HTML/masters_speed.html
- ChamSys: https://info.chamsyslighting.com/article/715-using-dj-integration-to-set-bpm-to-speedmasters ; https://docs.chamsys.co.uk/magicq/manual/OSC.html
- Avolites Titan: https://manual.avolites.com/docs/running-the-show/linking-pioneerdj-system-to-titan
- TCNet: https://en.wikipedia.org/wiki/Showkontrol ; https://npmjs.com/package/node-tcnet
- Daslight workaround: https://community.enginedj.com/t/sync-prime-4-with-daslight-dvc4-midi-sync/36706 ; Onyx Link: https://support.obsidiancontrol.com/Content/Onyx_Manual/Programming/DyLOS/Ableton_Link.htm
- obs-websocket: https://pub.dev/packages/obs_websocket
- CUE sheets: https://www.mixgraph.io/glossary/cue-sheet
- PRS live reporting: https://prsformusic.com/royalties/report-live-performances
- XDJ-700 used pricing: https://equipboard.com/items/pioneer-xdj-700
- rekordbox PRO DJ LINK support: https://rekordbox.com/en/support/link
