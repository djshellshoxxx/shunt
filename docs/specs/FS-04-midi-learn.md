# FS-04: MIDI learn

Status: draft 0.1. Depends on FS-00 (ControlRegistry). Feature id `midilearn`. UI `63-midilearn.js`.

## Goal

Control Shunt from a footswitch, pad controller or any MIDI device without a
screen: cycle bar offset, toggle beat only, mark set start, and every action
later features register (nudge, profiles).

## Behaviour

- Any action in the ControlRegistry can be bound to one MIDI message.
- **Learn:** the user presses Learn next to an action; for 15 s the next
  eligible incoming message is captured and bound. A second Learn on the same
  action replaces its binding. Binding a message already bound to another
  action asks for confirmation (`replace=true`) or fails with 409.
- **Eligible messages:** Note On (velocity > 0) and Note Off, Control Change,
  Program Change. Ignored always: system real-time (0xF8 clock, 0xFE active
  sensing), SysEx, pitch bend, aftertouch.
- **Binding key:** `(channel 0..15 or "any", type, number)`.
- **Modes:** `trigger` (fires on press: Note On, or CC value >= 64 rising
  edge; release ignored), `toggle` (same trigger, action decides), `absolute`
  (CC/velocity 0..127 mapped to 0..1, for Value actions). Default per action
  kind: Trigger gives `trigger`, Toggle gives `toggle`, Value gives `absolute`.
- A sustain-style footswitch that sends CC 64 = 127 on press and 0 on
  release works with `trigger`. 
- No feedback (LEDs) in v1.

## Input ports

`RawMidiIn` (Linux, `out/src/MidiIn.cpp`) opens an ALSA raw device
(`/dev/snd/midiC*D*`) read-only non-blocking and a `MidiParser` handles
running status, interleaved real-time bytes and truncated messages. The
input is polled from the Core net loop at 1 ms equivalence via a dedicated
small thread `MidiInThread` (read with `poll(2)`, 20 ms timeout) so it does
not depend on the network loop. Setting `port` selects the device; the same
physical device may be used as output by FS-existing MIDI clock (separate fd).
macOS and Windows ports arrive with the JUCE shell; the parser and binding
logic are platform independent and fully tested.

## Settings (`features.midilearn`)

```json
{ "enabled": false, "port": "/dev/snd/midiC1D0",
  "bindings": [ { "action": "baroffset.cycle", "channel": "any", "type": "note", "number": 60, "mode": "trigger" } ] }
```

Validation: unknown action ids are kept but flagged `missing` (a feature may
register later); duplicates by key are resolved last-wins on load with a
warning in status; at most 128 bindings.

## HTTP (`/api/f/midilearn/`)

| Route | Body | Result |
|---|---|---|
| `GET state` | | `{ports, port, learning: {action, secondsLeft}|null, bindings, lastMessage}` |
| `POST learn` | `{action, replace?}` | starts learn; 409 if key conflict arises later |
| `POST cancel` | | cancels learn |
| `POST unbind` | `{action}` | removes |
| `POST port` | `{port}` | switches input |

`lastMessage` shows the last non-ignored message (type, channel, number,
value) so users can see the controller working before binding.

## UI

Page `#/midilearn` (under Outputs for nav): port picker, a live "last
message" indicator, table of actions grouped by `group` with binding text
("Note 60, ch any"), Learn / Clear buttons, countdown while learning, and
the confirmation prompt for conflicts as an inline row (no modal).

## Design

`MidiParser` (pure), `BindingTable` (pure: `match(msg) -> vector<Hit>`,
`add/remove`, JSON in/out), `MidiLearn` (feature): owns thread, learn state
(mutex), registry pointer. Dispatch calls `controls().invoke()` on the input
thread (actions are thread-safe by FS-00).

## Tests

- ML-T1 parser: running status, real-time interleave, truncated, SysEx skip.
- ML-T2 trigger edge: CC 127 then 127 again fires once; 0 then 127 fires.
- ML-T3 absolute mapping bounds.
- ML-T4 learn capture, timeout, replacement, conflict 409 and `replace`.
- ML-T5 persistence and missing-action flag.
- ML-T6 end to end with a pipe or `socketpair` as the fake device: bytes in
  produce an action call; thread shuts down cleanly (no hang on stop).
- ML-T7 API shapes and validation limits.

## Acceptance and effort

A USB footswitch cycles the bar offset on a Pi and a Linux laptop; stop and
restart leave no stuck thread (TSan clean). Effort: M (3 to 4 days).
