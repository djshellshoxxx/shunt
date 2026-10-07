# RS-03: Desktop app and UX (regular spec)

Status: draft 0.1. Research: `docs/research/03-timing-appliance-integrations.md` section 3.

## Platforms

macOS 12+ (universal), Windows 10+, Linux x64 and aarch64. JUCE 8 GUI app
with the same engine and outputs as the Box.

## Behaviour

- Starts to the tray or menu bar; main window on click. Auto-start option.
- First run: interface picker showing which interface sees player
  keep-alives; mode picker (Follow default, Passive when rekordbox is
  detected on this host); firewall guidance per OS (installer adds a
  Windows rule).
- Main screen per ES-04 section 5. Stage mode shows only the valve (BPM,
  bar:beat, confidence), master name and output status, readable from 2 m.
- Phase scope page: last 64 residuals, jitter RMS, chosen latency offset,
  one-click "set latency from scope median".
- Tracklist panel: live rows, "set start" marker, export menu (CSV, CUE,
  timestamp text, NDJSON, performance report CSV).
- Compatibility page.
- Diagnostics: packet counters per device, timestamp source, record
  session to pcapng, open a replay.
- MIDI learn on bar offset, beat-only and follow controls; OSC input for
  the same (`/shunt/baroffset i`, `/shunt/beatonly i`).

## Settings storage

User settings in `ApplicationProperties`: interface, mode, device number
range, outputs, profiles, latency offsets, bar-offset memory table, export
defaults, window state. Session captures and tracklists in the user
documents folder.

## Accessibility

Keyboard for everything, 14 px minimum text, high-contrast theme only,
no modal dialogs while locked except the exit confirmation.
