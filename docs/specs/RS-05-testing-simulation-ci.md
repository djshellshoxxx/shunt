# RS-05: Testing, simulation and CI (regular spec)

Status: draft 0.1. Research: `docs/research/03-timing-appliance-integrations.md` section 6.

## Layers

1. **Unit tests** (Catch2, `ShuntTests`): packet parsers and builders
   (ES-02 P-T1 to P-T4), device table, claim and master state machines
   (ES-01 N-T3 to N-T6), clock engine (ES-03 C-T1 to C-T10), tracklist
   rules, exporters.
2. **Replay tests**: pcapng replays through the full stack with golden
   JSON (ES-01 N-T2). Captures: dysentery's `powerup.pcapng` and
   `to-virtual.pcapng`, plus our own from rented booths, stored with a
   manifest; large files fetched by script.
3. **ShuntSim**: a C++ tool that emits a scripted 4-deck plus mixer
   network on real UDP sockets (announce 1.5 s, status 200 ms, beats at the
   effective tempo) with scenarios: handoff 128 to 130, nudge, pitch ramp,
   hot cue, 4-beat loop, pause and resume, loss, jitter sigma 0.5 / 5 / 30
   ms with 1 percent 100 ms outliers. Used in CI against the real sockets
   on localhost and for manual testing of outputs.
4. **Output tests**: shared with Pacemaker (MIDI clock loopback jitter,
   Link two-process, OSC timetags).
5. **Hardware in the loop**: one XDJ-700 or XDJ-1000MK2 plus a dumb switch
   on the bench; rekordbox in Performance mode on a second laptop as a free
   peer. A monthly session in a real booth with a CDJ-3000 pair and a DJM
   when possible.

## CI

- `ci.yml`: Linux build of the core libs, unit tests, replay tests and a
  ShuntSim localhost run on every PR; aarch64 cross-build and tests under
  QEMU user mode.
- `release.yml`: Windows, Linux x64, Linux aarch64 and macOS universal
  desktop builds; Box image build job producing a flashable `.img.xz`;
  release on `v*` tags; macOS notarisation on tags.

## Release checklist

Version bump, changelog, CI green, hardware-in-the-loop session logged,
compatibility page updated for any newly verified model, legal wording
present in About and README.
