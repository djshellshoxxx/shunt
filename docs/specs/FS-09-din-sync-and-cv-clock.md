# FS-09: DIN sync and CV clock (Shunt Box)

Status: draft 0.1. Depends on FS-00; semantics follow the MIDI clock stream
(and therefore FS-01 mute). Feature id `pulse`. UI `68-pulse.js`. Box only.

## Goal

Clock Roland DIN sync gear, Korg and other pulse-input boxes and Eurorack
from GPIO on the Pi, locked to the same timeline as MIDI clock.

## Outputs

Three GPIO lines (BCM numbering, configurable):

| Line | Meaning |
|---|---|
| `clock` | pulse train at `ppqn` pulses per quarter note |
| `run` | level: high while running (Start/Continue), low when stopped |
| `reset` | single pulse on bar 1 when `barKnown` and `resetOnBar` (optional, -1 disables) |

`ppqn` choices: 1, 2, 4, 24 (Roland DIN sync), 48. Presets in the UI: "Roland
DIN sync (24)", "16th pulse (4)", "8th pulse (2)", "Quarter (1)", "48". The
spec author could not verify every vendor's pulse rate from this repo:
**the preset names must be checked against each manual before release** (task
D0 in the build plan), the numeric setting is authoritative.

Pulse width default 5 ms (range 0.5 to 20 ms, never more than 40 percent of
the pulse period; the effective width is clamped and reported). `polarity`
high (default) or low-active. `runLevel` as `polarity`.

## Driving the lines from the existing transport stream

No new clock logic. A `PulseGenerator` consumes the `MidiMsg` list that
`MidiClockGenerator::poll` already produces (so Start/Stop/Continue/mute/
reset behaviour is identical and FS-01 works for free):

- `0xF8` (24 ppqn tick): increments a tick counter; emit a clock pulse when
  `counter % (24/ppqn) == 0` for ppqn <= 24; for 48, emit a pulse at each tick
  and one at the midpoint to the next tick (midpoint = previous tick period / 2,
  scheduled when the tick is seen; first tick after start has no midpoint
  until a period is known).
- `0xFA` Start: counter = 0, `run` rises, `reset` pulse if configured, first
  clock pulse on the Start tick.
- `0xFB` Continue: `run` rises, counter continues.
- `0xFC` Stop: `run` falls, no further pulses.
- Output emits `Edge{timeNs, line, level}`; falling edges are scheduled at
  `rise + width`. Edges are written by the same output thread slot as MIDI
  messages (before sleeping to the next wake), spin-waiting the last 150 us.

The generator is independent of the MIDI port being enabled: the output
manager always runs `MidiClockGenerator` when either MIDI or pulse is on and
fans the message list out to both sinks. This is the one change to
`OutputManager` (small, isolated function `dispatchMidiMessages`).

## GPIO backend

`IPulsePort { bool set(line, level); bool open(chip, lines[]); void close(); }`

- `GpioCdevPort`: Linux GPIO character device uAPI v2, no library:
  `/dev/gpiochipN`, `GPIO_V2_GET_LINE_IOCTL` once for all three lines as
  outputs, `GPIO_V2_LINE_SET_VALUES_IOCTL` per edge (one ioctl can set several
  lines). Pi 4 header is usually `gpiochip0`; Pi 5 header is `gpiochip4`
  (RP1); the chip path is a setting and the UI offers detected chips.
- `MemoryPulsePort` for tests, records `(timeNs, line, level)`.
- Built only on Linux; other hosts report `available:false`.

## Defaults and pin plan

clock BCM17, run BCM27, reset BCM22. Chosen to avoid UART MIDI (14/15), I2C
OLED (2/3), the WS2812 data pin and button planned in RS-02 (18 and 4 are
reserved for them), and SPI. Pisound occupies most of the header: not
compatible with this feature unless pins are remapped (documented).

## Hardware notes (RS-02 addendum, deliverable of this feature)

- GPIO is 3.3 V and weak. DIN sync and most Eurorack clock inputs expect 5 V
  or more: buffer with 74AHCT125 (or 74HCT245) powered at 5 V, 220 ohm series
  resistor on each output, 5-pin DIN socket wired per Roland DIN sync
  (pin 1 clock, pin 2 ground, pin 3 run/stop; verify against the target
  device manual) and a 3.5 mm TS jack for Eurorack.
- Provide a schematic in `docs/hardware/pulse-out.md` (text and ASCII) and a
  BOM line (about $3).
- Lines must fail safe: driven low at start-up and shutdown. systemd unit
  gets `ExecStopPost` that releases the lines; the Box image sets pull-downs
  in `config.txt` for the chosen pins.

## Settings (`features.pulse`)

```json
{ "enabled": false, "chip": "/dev/gpiochip0", "clockLine": 17, "runLine": 27,
  "resetLine": 22, "ppqn": 24, "pulseMs": 5.0, "polarity": "high",
  "resetOnBar": true }
```

Validation: lines 2..27, distinct, `-1` allowed for run and reset; unknown
`ppqn` falls back to 24; `pulseMs` clamped as above; changing chip or lines
re-opens the port.

## Status

`{available, enabled, connected, error, ppqn, pulseMs, effectivePulseMs,
sent, jitterUs, running}`.

## API

`POST /api/f/pulse/test {seconds: 1..30}` drives 120 BPM test pulses
(clock, run high, reset every 4 beats) regardless of the timeline, with a
visible countdown; refuses when the transport is currently running (409) so a
test never injects into a live set.

## UI

Outputs page card (slot `outputs-midi` neighbour): enable, preset select,
ppqn, pulse width with effective value, polarity, pin pickers with the
default pin diagram, Test button, status dot and jitter. Hidden with an
"only on the Shunt Box" note when `available:false`.

## Tests

- PU-T1 `PulseGenerator` from synthetic MIDI streams: ppqn 1/2/4/24/48 pulse
  counts and spacing over 4 beats; midpoint timing for 48.
- PU-T2 Start/Stop/Continue run line; reset pulse at start and each bar when
  `barKnown`, none when unknown.
- PU-T3 pulse width clamp at 40 percent for fast tempos (e.g. 240 BPM, ppqn 48).
- PU-T4 polarity inversion. PU-T5 mute from FS-01 (Stop in stream) holds `run`
  low and no pulses.
- PU-T6 settings validation, pin conflicts.
- PU-T7 `GpioCdevPort` argument packing against a fake ioctl layer (struct
  sizes and flags checked, no hardware).
- PU-T8 test endpoint refusal while running.
- Hardware procedure `docs/hardware/pulse-jitter-test.md`: loop `clock` to a
  second GPIO input, log edge timestamps with the uAPI edge events for 10
  minutes at 120 BPM, report mean, p99 and max jitter, with `SCHED_FIFO` 90.

## Acceptance and effort

On a Pi 4 jitter p99 < 0.3 ms, max < 1 ms at 120 BPM over 10 minutes; DIN
sync gear (a real device or a logic analyser) locks and follows Stop/Start.
Effort: M (3 days software, 1 day hardware bring-up). Risk: unverified
vendor pulse rates (D0).
