# ES-05: Output engine (engineering spec)

Status: draft 0.1. Extends RS-01 (which defers to the Pacemaker repo). This
spec makes Shunt's `shunt_out` library self-contained and testable with no
JUCE, no sockets and no MIDI drivers in the unit tests. Inputs are the
`Timeline` and net `Event`s (ES-03). Platform I/O sits behind small sinks.

## 1. Module layout (`out/`)

| File | Responsibility |
|---|---|
| `MidiClock.h/.cpp` | `MidiClockGenerator`: timeline to timestamped MIDI realtime bytes via `IMidiSink` |
| `Osc.h/.cpp` | OSC 1.0 encoder (`i f s`, bundles), `OscOutput` with profiles, sends via `IOscTransport` |
| `LinkOutput.h/.cpp` | `LinkOutput`: applies `PublishPolicy` and quantum rule to an `ILinkBackend` |
| `OutputStatus.h` | `OutputStatus` shared by all outputs (enabled, connected, lastError, counters) |

All classes are single-threaded: the owner calls `update(...)` from one
scheduler thread. Sinks may hand off to other threads.

## 2. MIDI clock (`MidiClockGenerator`)

- 24 ticks per beat (0xF8). Tick `n` of beat `k` is at
  `timeOfBeat(k) + n/24 * (timeOfBeat(k+1) - timeOfBeat(k))`.
- `update(tl, nowNs, horizonNs, sink)` emits every message due in
  `(now, now + horizon]` exactly once, each with an absolute monotonic
  timestamp so the sink can use hardware timestamps. Default horizon 20 ms.
- Numbering is time-based, not index-based: the next tick is the first tick
  whose time is later than the last emitted tick by at least half a tick
  period and not earlier than `now`. This survives beat-index rebases.
- No output until `tl.bpm > 0` and `tl.playing` has been true once.
- Start (0xFA): at the first beat after first lock. After a reset, only if
  `startOnReset` (default off).
- Pause (`tl.playing == false`): `PauseMode::KeepRunning` (default) keeps
  ticking at the last tempo; `PauseMode::Stop` sends 0xFC once and silences
  ticks. Resume after Stop sends Continue (0xFB) on the next beat.
- Reset with `sppOnReset`: emit Song Position Pointer (0xF2 lsb msb, units
  of 16th notes, `(beatInBar-1)*4` of the upcoming beat, 0 if `!barKnown`)
  followed by Continue on the next beat. SPP and Continue are sent at the
  beat time, before that beat's first tick.
- Tempo is never quantised by the generator; the Timeline already carries
  the deadband policy via `PublishPolicy`.
- Test IDs: O-T1 tick spacing and count; O-T2 tempo change continuity;
  O-T3 Start on first lock, none on reset by default; O-T4 Stop/Continue;
  O-T5 SPP on reset; O-T6 no duplicate emission across overlapping calls.

## 3. OSC (`OscOutput`)

- Encoder: OSC 1.0, 4-byte padded strings, big-endian `i` and `f`, bundle
  with immediate time tag (`0x0000000000000001`). Pure functions, tested
  against hand-computed bytes (O-T7).
- `IOscTransport::send(const uint8_t*, size_t)`; the concrete UDP transport
  wraps `net::IUdpSocket` (`UdpOscTransport`).
- Profiles: `Generic`, `Resolume`, `GrandMA3`, `MagicQ`, `QLab`.
- Messages (root `/shunt`):
  - `/shunt/bpm f` on tempo publish (policy: 0.02 BPM, 1 s, or reset).
  - `/shunt/beat i beatInBar i barKnown` on each beat (emitted when now passes
    the beat time, with at most one message per beat).
  - `/shunt/transport i` 1 playing, 0 paused (on change).
  - `/shunt/master i device` on master change.
  - `/shunt/track s title s artist i deck` and `/shunt/track/id i rekordboxId`
    on load (caller supplies metadata; ids come from status events).
  - `/shunt/onair i deck i state` on change per channel.
- Profile mapping: Resolume sends `/composition/tempocontroller/tempo f`
  with a normalised value `(bpm-20)/(500-20)` clamped 0..1 and `/composition/
  tempocontroller/resync i 1` on bar downbeats when `barKnown`. GrandMA3
  sends `/gma3/cmd s "Master 3.1 At BPM <bpm to 2dp>"`. MagicQ sends
  `/magicq/bpm f`. QLab sends nothing on tempo; master change and track
  load map to user-configured cue numbers `/cue/<n>/start`. Generic sends
  the `/shunt/*` set. Every profile additionally sends the `/shunt/*` set
  unless `shuntMessages = false`.
- Tests: O-T8 profile message selection; O-T9 tempo throttling; O-T10 one
  beat message per beat with no duplicates.

## 4. Link (`LinkOutput`)

Ableton Link needs a licence (project plan Phase 0). The code is written
against `ILinkBackend` so the real Link SDK is a drop-in and the licence
does not block builds.

```
ILinkBackend: setTempo(bpm), requestBeatAtTime(beat, ns, quantum),
              forceBeatAtTime(beat, ns, quantum), beatAtTime(ns, quantum),
              setPlaying(bool, ns), numPeers()
```

- Quantum: 4 when `barKnown`, else 1.
- Tempo: `PublishPolicy::shouldPublishTempo` gates `setTempo`.
- Phase: once per beat compute
  `err = wrapped(beatAtTime(now) - expectedBeat)` where expected is the
  Shunt beat position modulo quantum; `PublishPolicy::phaseDecision` maps to
  None / Soft (`requestBeatAtTime`) / Hard (`forceBeatAtTime`).
- Start/stop sync is optional (`syncStartStop`, default off).
- Tests: O-T11 quantum switch; O-T12 hard on reset, soft after two
  consecutive errors, none within tolerance; O-T13 tempo throttle.

## 5. Status

`OutputStatus { bool enabled, connected; std::string lastError;
uint64_t messagesSent; double jitterNsRms; }` per output; the UI reads a
snapshot (RS-03).
