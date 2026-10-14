# Shunt feature audit, 2026-10-14

Scope: `djshellshoxxx/shunt`. Base audited: branch `claude/clever-pascal-0ycnqw` at
`371eebafb5f963454f3744f6e072e19bc639f090` (draft PR #2), which sits on default branch
`claude/wizardly-mayer-1dyo4l` at `2e914d2da93f882dcd48f012b5415b8ced994885`. Docs-only change; no
app code, releases or credentials touched.

## 1. Headline findings

**Update, after consolidation.** The first version of this audit (against `371eeba`) found three divergent
branches with duplicated work. At the owner's request ("whatever is best, features from both") they were
merged on this branch: PR #1 (`zealous-turing-knnajn`) is the base for outputs, core runtime, web UI and
headless app; this branch adds `LICENSE`, the BPM sanity guard (`Timeline::usable()`), the QEMU test run and
the parser fuzz test; the `eager-mayer` branch contributes `shunt_meta` (dbserver client, streaming XML
reader, fake-player tests) wired into `Core`, plus the Windows installer, MinGW toolchain and docs.
Duplicate implementations from the earlier PR #2 (`MidiClock`, `LinkOutput`, `OscInput`, `RawMidiSink`, ES-05)
were dropped in favour of PR #1's equivalents. The matrix below reflects the first pass; rows changed by
the consolidation are listed in section 6.

1. **Open (P0):** `eager-mayer` CI/release workflows were not imported: they target a JUCE desktop
   target that exists on no branch.
2. **Licence** is all rights reserved (`LICENSE`); `LEGAL.md` carries both the licence line and the required wording.

## 2. Method and evidence

Sources read: README, `docs/PROJECT-PLAN.md`, ES-01 to ES-05, RS-01 to RS-06, DIFFERENTIATION, all source
under `net/ clock/ log/ out/ tools/`, `tests/`, `.github/`, open PRs and all four remote branches (diffstat only
for non-audited branches). GitHub issues: none open.

Commands and results (Linux, g++, CMake Release):
- `cmake -S . -B /tmp/b && make -j8` builds with no warnings (`-Wall -Wextra`).
- `/tmp/b/ShuntTests` : **57 test cases, 1482 checks, 0 failures.**
- `ctest` in `/tmp/b`: **2 of 2 passed** (`ShuntTests`, `shuntsim_localhost` which runs a real-UDP handoff
  scenario, 13 s).
- Same suite under `-fsanitize=address,undefined`: 0 failures, no sanitizer reports.
- CI `ci.yml` (linux job and aarch64 QEMU job) completed with no failing suites on `371eeba`; the aarch64 job
  is the first run of that job.
- Grep for TODO/FIXME/stub in source: one hit, `net/src/Packets.cpp:307` (see gap G3).
- Not run: anything against real players, Windows/macOS builds, Link SDK, real MIDI hardware.

## 3. Feature matrix

Classes: **V** verified complete, **U** implemented, untested for the real world, **P** partial, **S** stub,
**M** missing, **L** planned only. Evidence is `file:line` plus the test IDs.

| # | Feature (spec) | Class | Evidence |
|---|---|---|---|
| 1 | Packet parsers/builders, all ports (ES-02) | V | `net/src/Packets.cpp`; P-T1..P-T5 incl. 20k fuzz |
| 2 | Device table, model inference, capability flags (ES-01 s4) | V (synthetic) | `net/src/DeviceTable.cpp:36`; device table test |
| 3 | Follow mode claim, safe numbers 5..., 7/8 fallback (ES-01 s5) | V (synthetic) | `net/src/ClaimMachine.cpp`; N-T3 x4 |
| 4 | CDJ-3000 compatible keep-alive byte 0x35=0x64 | V (golden) / U (hardware) | `net/src/Packets.cpp:299`; N-T4. Not checked against a CDJ-3000 |
| 5 | Passive mode never transmits (N6) | V | `net/src/NetworkStack.cpp:53`; N-T6 |
| 6 | Lead mode claim | P | `net/src/ClaimMachine.cpp:56,104,128`; N-T3 fail case only; no dbserver use |
| 7 | Master tracker, handoff, mixer master | V (synthetic) | `net/src/MasterTracker.cpp`; N-T5 |
| 8 | Socket layer, SO_TIMESTAMPNS, interfaces | U | `net/src/PosixSocket.cpp`; loopback ctest. POSIX only: no Windows |
| 9 | pcapng record and replay | V | `net/src/Pcapng.cpp`, `tools/shunt_cli/main.cpp`; round-trip test |
| 10 | Replay of real captures (N-T2) | M | captures not in repo (plan Phase 0) |
| 11 | ShuntSim scenarios (RS-05) | P | `tools/shuntsim/main.cpp`: handoff scenario used by ctest; other RS-05 scenarios not confirmed |
| 12 | Clock engine: estimator, resets, state machine (ES-03) | V (synthetic) | `clock/src/ClockEngine.cpp`; C-T1..C-T10 (C-T2 threshold amended to 5 ms) |
| 13 | Latency offset | V | `ClockEngine.cpp:185`; config only, no UI |
| 14 | Beat-only / barKnown rules, Opus, mixer, non-rekordbox | V | `ClockEngine.cpp:149`; bar offset test |
| 15 | Bar offset memory keyed by (rekordbox id, slot) (ES-03 s5) | M | no implementation; only the global offset (`ClockEngine.h` `setUserBarOffset`) |
| 16 | Publish policy: 0.02 BPM / 1 s / reset; phase soft/hard | V | `ClockEngine.cpp:295-322`; C-T10 |
| 17 | MIDI clock generator, Start/Stop/Continue/SPP, pause modes (ES-05 s2) | V (loopback to file) | `out/src/MidiClock.cpp`; O-T1..O-T6. No platform port |
| 18 | MIDI platform output: ALSA/UART raw device | U | `out/src/RawMidiSink.cpp`; O-T16 writes to a file only |
| 19 | MIDI virtual port, CoreMIDI, WinMM | M | planned (needs JUCE or SDKs) |
| 20 | OSC encode/decode, profiles, tempo throttle, beat msgs (ES-05 s3) | V | `out/src/Osc.cpp`, `OscInput.cpp`; O-T7..O-T10, O-T14 |
| 21 | OSC UDP transport | U | `out/include/shunt/out/Osc.h` `UdpOscTransport`; no test sends over a socket |
| 22 | OSC input control (`/shunt/baroffset`, `/beatonly`) | P | decoder and dispatch verified (O-T15); nothing binds a socket or calls the engine |
| 23 | OSC extra messages master/track/onair | V (unit) | `Osc.cpp` `onEvent`, `onTrackLoaded`; track metadata supplier does not exist on this branch |
| 24 | Link adapter, quantum 4/1, soft/hard (ES-05 s4) | P | `out/src/LinkOutput.cpp`; O-T11..O-T13 against a fake. **No real Link backend** (licence pending) |
| 25 | Output status block | P | `OutputStatus.h` exists; jitter not measured, no UI |
| 26 | Tracklist: load/on-air/unload rows, played rule | V | `log/src/Tracklist.cpp`; tracklist tests |
| 27 | Exporters: CSV, CUE, timestamp text, NDJSON, performance report | V (unit) | `Tracklist.h:65-70`; exporter test |
| 28 | Title/artist via dbserver / rekordbox XML | M here | not on this branch; present in PR #1 (XML) and `eager-mayer` (dbserver + XML) |
| 29 | Compatibility badges (Full/Tempo-only/Unsupported/Unverified) | P | flags in `DeviceTable.cpp:36-62`; no UI or report |
| 30 | CLI console (join, replay, record, selftest) | V | `tools/shunt_cli/main.cpp`; ctest selftest |
| 31 | Desktop app (tray, main window, stage mode) | M here / PR #1 has web UI only | no JUCE app anywhere |
| 32 | Headless runtime, web UI, WebSocket, OBS overlay | M here | PR #1 only, unmerged |
| 33 | Phase scope data | M here | PR #1 has a scope page |
| 34 | Venue profiles, MIDI learn, key clash, phrase countdown, on-air clock mute, DIN/CV | L | specs FS-01..FS-09 exist only on PR #1 |
| 35 | Appliance image (Pi), UART DIN, OLED | L | RS-02; nothing built |
| 36 | Release workflow, installers, notarisation | M here | `eager-mayer` has Windows/macOS scaffolding; not merged |
| 37 | Aarch64 CI | V on CI (first run) | `.github/workflows/ci.yml` |
| 38 | Licence and legal wording | V | `LICENSE`, `LEGAL.md`, README, RS-06 |

## 4. Gaps and priorities

| ID | Pri | Gap | Depends on | Acceptance test |
|---|---|---|---|---|
| G1 | P0 | Reconcile PR #1, `eager-mayer`, PR #2: one `shunt_out`, one `LEGAL.md`, one CMake layout | owner decision | single branch builds; union of tests (>=57 cases) passes in CI |
| G2 | P0 | Wire outputs into an executable (`shunt_headless` or `shunt_cli --outputs`) | G1 | `shuntsim` run produces MIDI bytes and OSC packets matching ES-05 from the live stack |
| G3 | P1 | Verify keep-alive 0x25/0x35 bytes against a real CDJ-3000 capture; remove TODO at `Packets.cpp:307` | hardware or capture | golden bytes from capture equal builder output |
| G4 | P1 | Replay tests N-T2 with real captures | captures | golden JSON stable across runs |
| G5 | P1 | Bar offset memory per (rekordbox id, slot) | metadata path | change offset on track A, load B, reload A: offset restored |
| G6 | P1 | Real Link backend behind `ILinkBackend` | Link licence | two-process test: receiver phase error < 3 ms RMS |
| G7 | P1 | Platform MIDI: CoreMIDI, WinMM, virtual port | SDKs or JUCE | loopback jitter < 1 ms RMS per platform |
| G8 | P1 | Windows socket layer (POSIX only today) | G1 | `ShuntTests` and selftest pass on MSVC/MinGW |
| G9 | P2 | Title/artist resolution (dbserver, rekordbox XML) | merge from `eager-mayer`/PR #1 | track rows carry title and artist in a replay |
| G10 | P2 | Output jitter measurement and status UI | G2 | `OutputStatus.jitterNsRms` populated and shown |
| G11 | P2 | OSC input socket and binding to engine | G2 | UDP packet to port changes bar offset in a running core |
| G12 | P2 | Remaining ShuntSim scenarios per RS-05 | none | each scenario has a ctest |
| G13 | P2 | Desktop shell (tray, stage mode) | JUCE | per RS-03 UX checklist |
| G14 | P3 | FS-01..FS-09 extras, Pi image, TCNet/StageLinQ/LTC | G1, G2 | per their specs |

## 5. Limitations

- Everything verified is synthetic or loopback; no real CDJ, XDJ or DJM was available. "Verified complete"
  means the specified behaviour passes its tests, not that it works in a club.
- Branches other than this one were inspected by diffstat and PR text only; their code was not built or run.
  Their claims (52 tests, jitter figures) are reported as stated by their PR descriptions, not reproduced.
- Windows, macOS and the Link SDK were not exercised.
- Unmerged PRs are not described as released; nothing in this repository is released.

## 6. Changes from consolidation (rows superseded)

| Matrix row | New state | Evidence |
|---|---|---|
| 15 bar offset memory | V | `app/src/Core.cpp` (`barOffsetMemory`, from PR #1) |
| 17, 20, 24 MIDI clock, OSC, Link | V/U via PR #1 implementation (`out/src/Midi.cpp`, `Osc.cpp`, `Link.cpp`), tests O-T1..O-T6 | `ShuntTests` 56 cases pass |
| 18 raw MIDI ports | U via PR #1 `Midi.cpp` | |
| 22 OSC input | V: `OscInput` socket class bound in `Core::syncOscInput` | `app/src/Core.cpp` |
| 28 title/artist | P: dbserver client wired in `Core` (tested only against `FakeDbServer`), XML import via PR #1 parser | `ShuntMetaTests` 13 cases, 137 checks |
| 31-33 app, web UI, scope | V (web UI via PR #1, loopback tested A-T3); no JUCE shell | |
| 36 installers | P: NSIS script and MinGW toolchain imported, not built in CI | `installer/`, `scripts/build-windows.sh` |
| Test totals | `ShuntTests` 56 cases / 1571 checks, `ShuntMetaTests` 13 / 137; ctest 3 of 3; ASan+UBSan clean | |

Still open: G2 is largely closed (`shunt_headless` drives outputs); G3, G4, G6, G7, G8, G13 unchanged.
