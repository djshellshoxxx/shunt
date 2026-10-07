# RS-01: Outputs (regular spec)

Status: draft 0.1. Shunt shares its output code with Pacemaker. The
detailed behaviour of each output is specified in the Pacemaker repo,
`docs/specs/ES-02-clock-outputs.md`; this file lists only what differs.

## Shared library

`clockout` is a library extracted from Pacemaker (Link publisher with
one-instance-per-process policy, MIDI clock scheduler thread with CoreMIDI
timestamps and spin-wait fallback, OSC sender with profiles, audio pulse
renderer). Both products depend on it. Shunt consumes `Timeline` (ES-03)
instead of Pacemaker's `BeatMapSnapshot`; a small adapter maps one to the
other (`beatOriginNs`, `bpm`, `beatInBar`, `confidence`, `resetSequence`
to `relock`).

## Differences from Pacemaker

| Area | Shunt behaviour |
|---|---|
| Link quantum | 4 when `barKnown`, else 1 (beat-only) |
| Link phase correction | soft via `requestBeatAtTime` when error over 1/60 beat for 2 beats; `forceBeatAtTime` only on `resetSequence` change |
| Tempo deadband | 0.02 BPM, 1 s minimum interval |
| MIDI clock on pause | default keep running at the last tempo; option Stop; Continue on resume with no SPP unless "SPP on reset" is enabled |
| MIDI Start | on first lock after start-up; on reset only if "Start on reset" is enabled (off by default; hardware resets patterns on Start) |
| OSC address root | `/shunt` |
| Extra OSC messages | `/shunt/master i device`, `/shunt/track s title s artist i deck` on load, `/shunt/onair i deck i state`, `/shunt/track/id i rekordboxId` |
| OSC profiles | Resolume (`/composition/tempocontroller/tempo`, `resync` on bar when `barKnown`), grandMA3 (`/gma3/cmd "Master 3.1 At BPM <v>"`), MagicQ generic, QLab (user-mapped cue numbers for master change and track load), Generic |
| Audio pulse | optional; only on desktop with an audio device or on the Box with Pisound |
| Host transport | none (Shunt is not a plugin) |

## Status reporting

Each output shows enabled, connected, last error and measured jitter in the
UI and in the WebSocket state.
