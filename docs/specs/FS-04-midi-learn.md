# FS-04: MIDI learn

Status: implemented. Depends on FS-00 (ControlRegistry). Feature id `midilearn`. UI `63-midilearn.js`.

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

## Deviations (as implemented)

1. Files: parser, `FdMidiInPort`, `MidiInThread` in `out/src/MidiIn.cpp`; `BindingTable`, `MidiLearnModel` (both pure, synthetic time) and the feature in `app/src/features/MidiLearn.{h,cpp}`. No ALSA library: the code base has none (clock output reads raw nodes), so input is a raw `/dev/snd/midiC*D*` fd (also accepts FIFOs, used by tests). ALSA seq input is not implemented.
2. The input thread polls with a 20 ms timeout and reopens a lost device every second; `MidiInThread` is not "polled from the Core loop".
3. 14-bit CC (MSB/LSB pairs) is not implemented; the spec gives no behaviour for it.
4. Learn binds channel `any` by default; body `exactChannel:true` binds the channel received. Channels are 0..15 in JSON and shown 1..16 in text ("CC 64, ch 1").
5. Overlap rule: two bindings conflict when type and number match and the channels are equal or either is `any`. One binding per action. On load, overlapping entries resolve last-wins with a warning.
6. Conflict flow: a learn that hits a conflict without `replace` ends with `lastLearn:"conflict"` and `state.conflict{action,text,with}`; `POST learn {action, replace:true}` for the same action then applies the captured message at once (the UI Replace button). `POST learn` itself returns 409 when the feature is disabled or 128 bindings exist.
7. `state` has extra keys: `enabled`, `open`, `inputError`, `lastLearn`, `conflict`, `actions` (id, label, kind, group), `warnings`; bindings carry `text`, `missing`. `status.features.midilearn` carries `port, open, inputError, learning, lastMessage, bindings, rev, messages` (the host owns `error`).
8. `POST port` accepts only an empty string or a port listed by `listMidiPorts()`. A port set in the settings file may be any absolute path (no `..`) to a character device or FIFO; regular files are refused.
9. The page is always in the nav (no `featureId`) because it holds the enable switch; order 22.
10. Mode is stored per binding; when absent it is derived from the action kind (Trigger, Toggle, Value gives trigger, toggle, absolute). Note absolute: Note On velocity/127, Note Off gives 0. Program change absolute: number/127.
11. Not verified on hardware: a real USB footswitch on a Pi or laptop, device unplug/replug on a real rawmidi node.
