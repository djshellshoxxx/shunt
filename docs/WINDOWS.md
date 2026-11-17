# Shunt for Windows

Shunt joins the player network of your CDJs / XDJs and DJM mixer, follows the
tempo-master deck, and sends its tempo, beat and bar to Ableton Link, MIDI
clock and OSC. It also writes down what was played and when.

## Requirements

- 64-bit Windows 10 or 11.
- A wired Ethernet connection to the same switch (or mixer hub) as the players.
  Wi-Fi works for Link and OSC, but the player network should be wired.

## Install

- **Installer** (`Shunt-<version>-windows-x64-setup.exe`): installs Shunt,
  adds the Windows Firewall rules for the player network (UDP 50000-50002),
  and creates Start Menu entries. Optional: start Shunt when you sign in.
- **Portable** (`Shunt-<version>-windows-x64-portable.zip`): unzip anywhere and
  run `Shunt.exe`. Windows Firewall asks once; allow both Private and Public
  networks (DJ booth networks are usually "Public" because they have no
  internet gateway).

## First run

1. Pick the network interface that sees the players. Shunt listens for a few
   seconds and marks interfaces where players were found.
2. Pick a mode:
   - **Follow** (default): Shunt joins the network as a silent extra device
     (number 7 or higher). Players are never asked to give up a slot.
   - **Passive**: Shunt sends nothing at all. Use this when rekordbox is running
     on the same computer.
   - **Lead (metadata)**: Shunt takes a real player number (1 to 6) so it can read
     track titles from the players. Only use a number that no player is using.
3. Turn on the outputs you need: Link, a MIDI clock port, OSC.

## Everyday use

- **Bar offset** (B): if your downbeat is on the wrong beat, press until it is
  right. Shunt remembers the offset per track.
- **Beat only**: send tempo and beat without bar phase.
- **Stage mode** (S): large BPM and bar:beat readable from a distance.
- **Record session**: saves a capture (`.pcapng`) of the network for replay and
  bug reports.
- **Tracklist**: export CSV, CUE, text, NDJSON or a performance report for
  rights reporting. Files are in `Documents\Shunt`.
- **OBS overlay**: add a Browser Source with `http://localhost:8080/overlay`.

## Included tools

- `shunt_headless.exe`: Shunt without a window, with a web page on port 8080.
- `shunt_cli.exe`: console monitor, recorder and replayer.
- `shuntsim.exe`: simulates a four-player network on this computer for testing
  (`shuntsim --dest 127.0.0.1`).

## Notes

- MIDI clock on Windows uses the regular Windows MIDI ports. Windows has no
  built-in virtual MIDI port; to send clock to another app on the same PC, use
  a loopback driver such as loopMIDI, or use Link instead.
- Settings: `%APPDATA%\Shunt\settings.json`.

## Legal

Shunt works with PRO DJ LINK compatible players and mixers from AlphaTheta /
Pioneer DJ. PRO DJ LINK, rekordbox and Pioneer DJ are trademarks of their
respective owners. Shunt is an independent product and is not affiliated
with, endorsed, licensed or certified by AlphaTheta Corporation or Pioneer
Corporation. See LEGAL.txt for third-party licences.
