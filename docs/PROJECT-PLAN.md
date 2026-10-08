# Shunt project plan and checklist

Phases are sequential; each ends with something usable. Keep this file
current; tick boxes as work lands.

## Phase 0: Foundations (1 to 2 weeks)

- [x] Research notes (`docs/research/01` to `03`)
- [x] Engineering specs ES-01 to ES-04; regular specs RS-01 to RS-06
- [x] CMake skeleton: `shunt_net`, `shunt_clock`, `shunt_log` (plain C++17, no JUCE), `ShuntTests` (self-contained test header instead of Catch2), `shuntsim`
- [x] CI: Linux build and unit tests on every PR (`ci.yml`)
- [ ] CI: aarch64 cross job
- [ ] Legal wording file (`LEGAL.md`) with the ES-04 section 6 text
- [ ] Ableton Link licence request (joint with Pacemaker)
- [ ] Obtain the two dysentery captures into `Tests/captures/` with a manifest

## Phase 1: Protocol core (2 to 3 weeks)

- [x] Packet parsers and builders per ES-02 with golden tests P-T1 to P-T4
- [x] Device table, model inference and capability flags (ES-01 section 4)
- [x] Claim state machine with CDJ-3000-compatible keep-alive (ES-01 section 5); tests N-T3, N-T4
- [x] Master tracker (ES-01 section 6); test N-T5
- [x] pcapng writer and replayer (`shunt_cli --replay`)
- [ ] Replay tests N-T2 (need the dysentery captures)
- [x] ShuntSim with the scripted scenarios (RS-05)
- [x] Socket layer with interface selection, broadcast, SO_TIMESTAMPNS, Passive mode; test N-T6

## Phase 2: Clock engine and outputs (2 to 3 weeks)

- [x] Estimator, reset rules, bar phase, state machine (ES-03); tests C-T1 to C-T10
- [x] `shunt_out` library written fresh against RS-07 (no Pacemaker code needed; tests O-T1 to O-T6)
- [x] Link output logic with quantum 4/1 and soft/hard phase policy over `ILinkSession` (mock-tested)
- [ ] Link SDK binding built and tested with `-DSHUNT_WITH_LINK=ON` (needs licence and SDK)
- [x] MIDI clock output with pause behaviour options (raw ALSA/serial ports)
- [ ] Virtual MIDI ports (CoreMIDI, WinMM/teVirtualMIDI, ALSA seq) in the JUCE shell
- [x] OSC output with Shunt messages and profiles
- [x] Latency offset and phase scope data

## Phase 3: Desktop app (3 to 4 weeks)

- [x] Main screen per ES-04 section 5 and stage mode as an embedded web UI (RS-07 section 9)
- [ ] JUCE shell: tray, auto-start, WebView, native MIDI ports
- [x] First-run interface and mode picker with firewall guidance
- [ ] Windows installer firewall rule
- [x] Tracklist logger, exporters, NDJSON persistence and `/overlay` page
- [ ] rekordbox XML import for titles and artists
- [x] Compatibility page; diagnostics page; record toggle
- [ ] Replay-a-capture UI
- [x] Bar offset memory
- [ ] MIDI learn, OSC input
- [ ] Release workflow: Windows, Linux x64 and aarch64, macOS universal; notarisation on tags

## Phase 4: Beta and v1.0 (3 weeks)

- [ ] Bench rig: XDJ-700 or XDJ-1000MK2 plus switch; rekordbox peer laptop
- [ ] Booth session with CDJ-3000 pair and DJM; capture stored
- [ ] Beta with 3 hybrid acts, 1 hardware DJ, 1 lighting operator, 1 promoter wanting tracklists
- [ ] Success metrics in ES-04 section 7 met
- [ ] Website with the compatibility page as the main content; legal wording
- [ ] v1.0 release

## Phase 5: Shunt Box (3 to 5 weeks)

- [x] `shunt_headless` with built-in HTTP/WebSocket server and web UI (RS-02, RS-07; no civetweb needed)
- [ ] Pi OS image: overlayfs, `/data`, systemd watchdog, NetworkManager profiles, avahi
- [ ] UART DIN MIDI and USB gadget mode; OLED; WS2812; button
- [ ] Image build in CI; signed update bundles
- [ ] Field test in a booth for a full night
- [ ] Image on sale; assembled box pilot batch of 10

## Later (v1.x and v2)

- [ ] WebSocket state and OBS overlay polish
- [ ] Phrase countdown from CDJ-3000 phrase data
- [ ] Mixer on-air to OSC and clock mute
- [ ] NFS metadata opt-in
- [ ] StageLinQ input
- [ ] Lead mode (CDJs follow Link)
- [ ] TCNet-compatible output after legal review
- [ ] LTC and MTC

## Risks and mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| Firmware changes break parsing | wrong tempo or no devices | capture and replay mode, modular parsers, fast update channel |
| Keep-alive bytes kick CDJ-3000s off the network | catastrophic in a club | golden tests on the 0x35 byte; Passive mode fallback; bench test on a CDJ-3000 before release |
| Opus Quad and XDJ-RX buyers expect full support | refunds, reviews | compatibility page first; tempo-only badge; no RX support promised |
| Link licence delay | no Link in commercial builds | feature flag; MIDI clock and OSC still ship |
| Venue IT bans unknown devices after the 2025 advisory | Box not allowed | read-only posture, no NFS by default, documentation for venues |
| Trademark complaint | rename or rewording | wording rules in RS-06; no logos; lawyer review before launch |
| No hardware on the bench | untested against real players | buy an XDJ-700 early; rekordbox peer rig; booth sessions |
