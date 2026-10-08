# RS-07: Outputs implementation, core runtime and web UI (regular spec)

Status: draft 0.1. Fills the gaps left by RS-01 (outputs), RS-02 (headless
runtime) and RS-03 (desktop app). Decision record at the end.

## 1. Libraries and executables

| Target | Contents | Depends on |
|---|---|---|
| `shunt_out` | OSC codec and sender, MIDI clock generator, raw MIDI ports, Link output logic over `ILinkSession`, `OutputManager` | `shunt_clock` |
| `shunt_app` | JSON, `Settings`, `Core` (threads), `HttpServer` (HTTP + WebSocket), embedded web UI | all libraries |
| `shunt_headless` | executable: `Core` + `HttpServer`; the Box service and the desktop engine | `shunt_app` |

No third-party code. No JUCE in this layer; the JUCE desktop shell (tray,
auto-start, native MIDI ports, Link SDK) wraps `shunt_app`.

## 2. MIDI clock generator (`MidiClockGenerator`)

Pure and time-driven so it is testable with synthetic time.

- Input: `Timeline`, options `{pause: KeepRunning|Stop, startOnReset, sppOnReset, startOnFirstLock}`.
- `poll(nowNs, lookaheadNs, tl, out)` appends `{timeNs, bytes}` for every
  message due before `now + lookahead`. Tick `n` is at beat `n / 24`, time
  `tl.timeOfBeat` extended to fractions: `origin + (n/24 - beatIndex) * period`.
- Never emits two messages less than 1 ms apart in the wrong order: a computed
  tick time earlier than the previous emitted time is clamped to it.
- Start (0xFA): on first lock, sent at the next tick that is a bar boundary
  when `barKnown`, else a beat boundary; then clocks (0xF8) follow. On
  `resetSequence` change: resynchronise the tick index to the new phase; send
  Start only when `startOnReset`; send Song Position Pointer (0xF2, in 16ths,
  position of the tick's bar start) before it when `sppOnReset`.
- Pause (`tl.playing` false): KeepRunning continues at the last tempo from the
  frozen phase; Stop sends 0xFC once and goes quiet; resume sends Continue
  (0xFB) at the next beat boundary (Stop mode only).
- No lock (`bpm <= 0`): silent.
- Thread wrapper `MidiClockThread`: wakes every 1 ms, asks the generator for
  messages in the next 3 ms, sleeps coarsely then spin-waits the last 150 us
  before writing each message, and records `sentNs - scheduledNs` for jitter
  (RMS over the last 512 ticks, shown in the UI).
- Ports: `IMidiPort::write(bytes, len)`. `RawMidiPort` opens an ALSA raw MIDI
  device node (`/dev/snd/midiC*D*`, also the Box UART via `/dev/serial0` after
  `stty 31250`); `MemoryMidiPort` for tests. CoreMIDI and WinMM ports arrive
  with the JUCE shell. `listMidiPorts()` enumerates `/dev/snd/midiC*D*`.

## 3. OSC output

- Codec: `oscMessage(address, args)` with `i`, `f`, `s` arguments, 4-byte
  padding, big-endian. Sent by UDP to `host:port` (default 127.0.0.1:9000).
- Events: tempo (policy `PublishPolicy::shouldPublishTempo`), beat (each beat,
  scheduled by the output thread against the timeline), bar (beat 1 when
  `barKnown`), transport (on `playing` edge), master change, track load.
- Profiles:

| Profile | Messages |
|---|---|
| Generic | `/shunt/bpm f`, `/shunt/beat i beatInBar`, `/shunt/bar i`, `/shunt/transport i`, `/shunt/master i`, `/shunt/track s s i`, `/shunt/onair i i`, `/shunt/track/id i` |
| Resolume | `/composition/tempocontroller/tempo f` (normalised `(bpm-20)/480`, clamped; verify against the installed Resolume version), `/composition/tempocontroller/resync i 1` on each bar when `barKnown` |
| grandMA3 | `/gma3/cmd s "Master 3.1 At BPM <v>"` on tempo publish |
| MagicQ | Generic messages plus `/bpm f` |
| QLab | `/cue/<n>/start` where `n` is the user-mapped cue for master change and for track load (settings `qlabMasterCue`, `qlabTrackCue`) |

## 4. Link output (`LinkOutput`)

Logic is written against `ILinkSession` (`setTempo`, `beatAtTime`,
`requestBeatAtTime`, `forceBeatAtTime`, `numPeers`, `enable`) so it is
testable with a mock. The Ableton Link SDK binding (`LinkSdkSession`) is
compiled only with `-DSHUNT_WITH_LINK=ON` and `Link.hpp` on the include path
(licence request is open, see the project plan). Without it the output
reports `available=false, error="Link SDK not built in"`.

- Quantum 4 when `barKnown`, else 1.
- Tempo: apply `tl.bpm` when `PublishPolicy::shouldPublishTempo`.
- Phase: once per beat compute `phaseErrorBeats = wrap(linkBeat - expectedBeat)`
  where the expected beat is derived from the timeline and quantum; ask
  `PublishPolicy::phaseDecision`: `Soft` calls `requestBeatAtTime`, `Hard`
  calls `forceBeatAtTime`.
- One Link instance per process.

## 5. Output manager

Owns the three outputs, one thread at 1 ms. Each tick: copy the timeline from
the core (mutex), call each enabled output, update `OutputStatus
{enabled, connected, error, detail}` (peers, jitter us, messages sent).
Settings changes rebuild only the affected output.

## 6. Settings

`Settings` serialised as JSON to `config.json` (path `--config`, default
`$XDG_CONFIG_HOME/shunt/config.json`, else `~/.config/shunt/config.json`).
Unknown keys are preserved on save; invalid values are clamped, never fatal.

```json
{
  "interface": "eth0", "mode": "follow", "deviceNumber": 5,
  "latencyMs": 1.0, "barOffset": 0, "beatOnly": false, "profile": "stage",
  "venue": "", "performer": "",
  "link":  {"enabled": false},
  "midi":  {"enabled": false, "port": "", "pause": "keep", "startOnReset": false, "sppOnReset": false, "startOnFirstLock": true},
  "osc":   {"enabled": false, "host": "127.0.0.1", "port": 9000, "profile": "generic", "qlabMasterCue": 1, "qlabTrackCue": 2},
  "barOffsetMemory": {"<rekordboxId>:<slot>": 2},
  "http":  {"port": 8080, "bind": "0.0.0.0"}
}
```

## 7. Core runtime

Two threads plus the HTTP thread:

1. **Net thread**: `stack.poll(5)`; drains events into `ClockEngine` and the
   tracklist; applies bar-offset memory when a track loads on the master;
   handles restart requests (interface, mode, number) by stopping and
   rebuilding the stack.
2. **Output thread** (`OutputManager`).
3. **HTTP thread**: `HttpServer`, calls into `Core` through thread-safe
   methods only.

State shared by mutex: timeline copy, status snapshot, tracklist session,
scope ring (last 64 residuals of the master, ms). The status snapshot is
rebuilt at 10 Hz and as JSON on demand.

Bar offset memory: when the user changes the offset while a track is loaded
on the master, store `(rekordboxId, slot) -> offset`; on the next load of that
key on the master apply it.

## 8. HTTP and WebSocket API

Single port (default 8080). All JSON UTF-8.

| Route | Purpose |
|---|---|
| `GET /` | web UI |
| `GET /overlay` | OBS overlay page (RS-04) |
| `GET /ws` | WebSocket: server pushes `{"type":"status", ...}` 10 Hz |
| `GET /api/status` | one status object |
| `GET /api/config`, `POST /api/config` | settings; POST merges and applies |
| `GET /api/interfaces` | `[{name, ip, mac, loopback}]`; the first-run banner shows live devices only for the interface in use (listening on every interface at once is later work) |
| `GET /api/midi-ports` | MIDI ports |
| `GET /api/tracklist` | rows and events |
| `GET /api/export?format=csv\|cue\|txt\|ndjson\|report` | download |
| `POST /api/baroffset` `{"value":0..3}` and `{"cycle":true}` | bar offset |
| `POST /api/beatonly` `{"value":bool}` | beat only |
| `POST /api/latency` `{"ms":x}` or `{"fromScope":true}` | latency |
| `POST /api/setstart` | "set start" marker for CUE |
| `POST /api/session/clear` | new tracklist session |
| `POST /api/record` `{"on":bool}` | pcapng recording to the data dir |
| `GET /api/compat` | compatibility table (research 02 section 4) |

Status object keys: `now, uptimeS, mode, interface, timestampSource, claim,
state, bpm, beatInBar, beatIndex, barKnown, confidence, playing, master,
masterName, devices[], outputs{link,midi,osc}, scope{residualsMs[],
rmsMs, medianMs, latencyMs}, counters, tracks, setStartMs, recording`.

The server binds `0.0.0.0` by default so the Box UI is reachable from the
config Wi-Fi; there is no authentication in v1 (read-write on the local
network is the same trust model as the players); the `bind` setting restricts
it. The UI never calls external hosts (no CDN, system font stack only).

## 9. Web UI

Single embedded HTML file (`ui/index.html`), vanilla JS, no framework, works
offline, 14 px minimum text, keyboard operable, dark high-contrast theme
(RS-03), responsive from 360 px phones (Box on Wi-Fi) to a 1080p stage screen.

Implementation notes: with no `interface` saved the core auto-selects the first non-loopback interface and reports `firstRun: true` until the user confirms one. The latency "centre on scope median" adds the scope median to the offset as RS-03 asks; it must be validated on hardware, because a converged filter leaves a median near zero.

Pages (tabs, hash routing `#/live`, `#/outputs`, `#/tracklist`, `#/scope`,
`#/compat`, `#/diagnostics`):

- **Live** (default): the circulation diagram. Left column: player tiles
  (number, name, model badge, play state, BPM, on-air ring, master crown).
  Centre: the valve: large BPM, bar:beat with four beat lamps (bar-1 lamp
  distinct), confidence ring, state label, `no bar` badge when `!barKnown`.
  Right column: output tiles (Link peers, MIDI port and jitter, OSC
  destination) with status dots. Connecting lines animate when active.
  Bottom strip: last tracklist rows. Control bar: bar offset 0..3 segmented
  control, beat-only toggle, latency stepper, Follow/Passive, profile,
  record, export menu.
- **Stage mode** (`S` key or button): full-screen valve only: BPM, bar:beat,
  confidence, master name, output dots; readable from 2 m (BPM at least
  18 vmin); `Esc` leaves.
- **Outputs**: forms for Link, MIDI (port picker, pause, start options),
  OSC (host, port, profile, QLab cues); changes apply on Save with inline
  result.
- **Tracklist**: live table, set-start marker, export buttons.
- **Phase scope**: canvas of last 64 residuals with +-1 ms and +-4 ms rules,
  RMS and median, latency offset stepper, "centre on median".
- **Compatibility**: table with badges (Full, Tempo only, Partial, Unsupported,
  Unverified) from `/api/compat`; a tile's model badge links here.
- **Diagnostics**: per-type packet counters, timestamp source, claim state,
  device list with last-seen ages, record toggle.
- First run (no `interface` in settings): modal-free banner on Live with the
  interface list, "players seen" marker, mode picker, firewall hint.
- Connection loss: banner "Engine not reachable", automatic reconnect with
  backoff; values dim after 1 s without data.

Accessibility: every control is a real button/input with a visible focus ring;
`aria-live=polite` for state changes; `prefers-reduced-motion` stops line
animation.

## 10. Tests

- O-T1 OSC encoding byte-exact for known messages (including padding).
- O-T2 MIDI generator at 120 BPM: tick spacing 20.833 ms +-1 us, 24 per beat;
  Start once on first lock, at a bar boundary when `barKnown`.
- O-T3 pause KeepRunning continues; Stop sends 0xFC then Continue on resume.
- O-T4 reset resyncs ticks without a Start unless `startOnReset`; SPP when
  `sppOnReset`.
- O-T5 Link logic with a mock: tempo deadband, quantum 4 vs 1, soft vs hard.
- O-T6 OSC profile mapping (Resolume normalisation, grandMA3 string).
- A-T1 JSON round trip, unknown keys preserved, invalid values clamped.
- A-T2 WebSocket accept key matches RFC 6455 example; frame encode/decode.
- A-T3 HTTP server integration: `/api/status` JSON, `/api/config` POST,
  `/api/export`, 404, WS upgrade and first status frame.
- A-T4 Core end to end on loopback with `shuntsim` is covered by the existing
  `shuntsim_localhost` style test for `/api/status` (bpm near 130).

## Decision record

**Web UI instead of a JUCE-only GUI for the first release.** One UI serves
the Box (phone and laptop over Wi-Fi), the desktop app (a thin JUCE shell
hosting a WebView or opening the browser) and OBS. The modern look, tests with
a real browser, and no JUCE dependency in CI follow from that. The JUCE shell
only adds tray, auto-start, native MIDI and the Link SDK. RS-03 behaviours are
therefore implemented in the web UI first.
