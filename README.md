# SHUNT

**Diverts the beat from the DJ booth into everything else.**

Shunt joins the player network used by AlphaTheta / Pioneer DJ players and
mixers, follows the tempo-master deck's tempo, beat and bar, and republishes
it as **Ableton Link** (with bar phase), **MIDI clock** (USB and DIN) and
**OSC**, so musicians playing alongside a DJ, hardware sequencers, lights
and video all stay on the one. It also logs exactly what was played and
when, for cue sheets and rights reporting.

Desktop app for macOS, Windows and Linux, and a headless Raspberry Pi
appliance ("Shunt Box"). C++17, JUCE 8, CMake, same toolchain as
[Vivisect](https://github.com/djshellshoxxx/faultline) and
[Pacemaker](https://github.com/djshellshoxxx/pacemaker).

Status: protocol core, clock engine, MIDI clock, OSC, Link logic (SDK binding
pending licence), tracklist logger and a web UI are implemented and tested.
The JUCE desktop shell and the Box image are next.

## Try it

```sh
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build --output-on-failure
./build/shuntsim --dest 127.0.0.1 --duration 600 &      # fake booth on loopback
./build/shunt_headless --addr 127.0.0.1                 # then open http://localhost:8080/
```

On a real network use `--iface eth0` (the interface wired to the players).
Pages: Live, Outputs, Tracklist, Phase scope, Compatibility, Diagnostics;
press `S` for stage mode. `/overlay` is an OBS browser source.

## Documents

- `docs/research/` — deep research notes with sources (`00-index.md`
  summarises them)
- `docs/specs/ES-01-network-stack.md` — network engineering spec
- `docs/specs/ES-02-packet-formats.md` — packet engineering spec
- `docs/specs/ES-03-clock-engine.md` — timing engineering spec
- `docs/specs/ES-04-product.md` — product engineering spec
- `docs/specs/RS-*.md` — regular specs: outputs, appliance, desktop app and
  UX, tracklist logging, testing and CI, licensing and legal
- `docs/specs/RS-07-outputs-core-and-web-ui.md` — outputs, core runtime, HTTP API, web UI
- `docs/DIFFERENTIATION.md` — what makes Shunt different, ranked extra
  features
- `docs/PROJECT-PLAN.md` — phases, checklist, risks

## Legal

Shunt works with PRO DJ LINK compatible players and mixers from AlphaTheta /
Pioneer DJ. PRO DJ LINK, rekordbox and Pioneer DJ are trademarks of their
respective owners. Shunt is an independent product and is not affiliated
with, endorsed, licensed or certified by AlphaTheta Corporation or Pioneer
Corporation. The protocol implementation is written from publicly
documented packet analysis; no code from EPL or GPL projects is used.
