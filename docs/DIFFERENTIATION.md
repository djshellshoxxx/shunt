# What makes Shunt different, and what else it could do

Research basis: `docs/research/02-market-competitors-hardware-legal.md`.

## 1. The field today

| | Pioneer PRO DJ LINK Bridge | ShowKontrol / ProDJLink.com | Beat Link Trigger + Carabiner | djlink2midi and similar | DJM MIDI clock | Shunt |
|---|---|---|---|---|---|---|
| Who it is for | certified lighting software | lighting and show control | programmers and lighting hobbyists | tinkerers | DJs with one hardware box | musicians next to a DJ, hardware DJs, small lighting, tracklists |
| Outputs | TCNet only | timecode, Link (BPM), MIDI clock, triggers | Link (bar aligned), OSC, triggers | MIDI clock (tempo only) | MIDI clock (tempo only, from the FX channel) | Link with bar phase, MIDI clock DIN and USB, OSC, tracklist |
| Needs | laptop with two NICs | laptop, $249 to $1,989 | Java 21, Ethernet, Carabiner daemon | Node, Pi | the mixer | one binary, or a box |
| Phase handling | n/a | BPM only | dead reckoning, hard jump over 1/60 beat; smoothing "unreleased" | none | none | filtered estimator, slewed handoff, reset only on true discontinuities |
| Hardware coverage | current flagships only | wide | wide | narrow | mixer only | wide, with honest badges |
| Tracklist | no | PRO tier lease | yes (expressions) | no | no | yes, exports for cue sheets and rights reporting |
| Price | free | $249 to €1,989 | free | free | included | $69; box $279 |

## 2. The five differentiators

1. **Musician-first outputs.** Link with beat and bar phase, MIDI clock on
   DIN and USB with Start and Continue aligned to the downbeat, OSC, from
   one native binary.
2. **The box.** A headless Pi in the booth switch with a phone web page. No
   shipped product does Pro DJ Link to Link and MIDI in a box.
3. **Handoff and jitter engineering as the product.** The reference
   implementation hard-jumps Link whenever the error exceeds 1/60 beat and
   documents smoothing as unreleased. Shunt's estimator, slew and reset
   rules are the core feature.
4. **Network-derived tracklist.** Exact on-air times, deck and source,
   exported as CSV, CUE, text and a rights-report sheet; nobody under $100
   does this from the network.
5. **Honest compatibility.** Full, tempo-only and unsupported badges with
   reasons, instead of silent drift or crashing an XDJ-RX.

## 3. Additional features, ranked by value divided by effort

| Rank | Feature | Why | Effort | Phase |
|---|---|---|---|---|
| 1 | WebSocket JSON state plus OBS browser-source overlay | parity with prolink-tools for free | low | 1.x |
| 2 | Phrase countdown (bars to next section) via CDJ-3000 phrase data | the thing live musicians ask for | medium | 1.x |
| 3 | Mixer on-air to OSC; mute clock when off air | lighting and hardware users | low | 1.x |
| 4 | StageLinQ input (Denon Prime) | Denon venues; community libs exist | medium | 2 |
| 5 | Unofficial TCNet-compatible output | drop-in for MagicQ and Arena users | medium-high, legal review | 2 |
| 6 | DIN-sync and CV clock on the box | Elektron and modular | low on the box | 1.x |
| 7 | LTC and MTC per deck | timecode shows | medium | 2 |
| 8 | Lead mode: CDJs follow Link | reverse sync; BLT has it | medium, risk | 2 |
| 9 | Key clash alarm between on-air and cued deck | small, novel, needs key metadata | low once metadata exists | 1.x |
| 10 | Device Library Plus decryption | legal risk | not planned | never |

## 4. Shared code with Pacemaker

The `clockout` library (Link publisher, MIDI clock scheduler, OSC profiles,
audio pulse) and the house UI theme are shared. A key-clash alarm and the
tracklist exporters are candidates for stand-alone spin-off utilities.
