# Research index

Deep research gathered on 7 October 2026 before writing the specs. Each
file keeps its own Sources list. Items flagged "unverified" or "via search"
must be re-checked against a real capture or primary document before being
relied on in code.

| File | Topic | Key conclusions |
|---|---|---|
| `01-pro-dj-link-protocol.md` | Ports, discovery, device-number claim, keep-alive bytes, beat and status packet layouts, mixer packets, master semantics, metadata paths, timing, libraries and licences | Everything is broadcast or unicast UDP on 50000 to 50002 with a 10-byte magic. Beat packets carry pitch, BPM and beat-in-bar once per beat; status packets carry master flags, handoff target, beat number and rekordbox id at 5 Hz. Keep-alive byte 0x35 must be 0x64 or CDJ-3000s drop off. Follow mode should use numbers 7 to 15. Clean-room C++ from the published analysis is licence-safe; the Link SDK is the licence to budget for. |
| `02-market-competitors-hardware-legal.md` | Official Bridge and TCNet, ShowKontrol, ProDJLink.com, rekordbox Link, DJM MIDI clock, Beat Link Trigger, small bridges, user pain points, compatibility matrix, pricing, trademarks, 2025 security advisory | Nothing under $249 publishes bar-aware Link and MIDI clock from the network; the official Bridge is TCNet-only. Loudest complaints: Ableton external sync drift, bar misalignment, needing a laptop and Java, handoff glitches, Opus Quad and XDJ-RX unsupported. No takedown history; wording rules matter. |
| `03-timing-appliance-integrations.md` | Beat-packet filtering, jitter numbers, events the filter must survive, bar phase mapping, Pi appliance design, desktop sockets, lighting integrations, tracklist formats, testing without hardware | The reference implementation does no filtering and hard-jumps Link; an alpha-beta estimator using the reported tempo for period and arrival time for phase, with reset only on true discontinuities, is the recommended design. Pi 4 plus UART MIDI is a $80 to $110 BOM. ShuntSim and pcapng replay make most testing hardware-free. |

Open questions carried into the specs:

- Exact claim-stage byte templates (capture from a real player).
- Precise-position packet absolute offsets.
- CDJ-3000X, CDJ-1500X and XDJ-AZ behaviour on the latest firmware.
- rekordbox device numbers on the wire.
- Measured wired jitter on real players (none published).
