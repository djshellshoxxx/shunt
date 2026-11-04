# Research 05: DIN sync and pulse clock rates (spike D0, risk R4)

Status: 4 November 2026. Input to `docs/specs/FS-09-din-sync-and-cv-clock.md`.

## 0. Read this first: what is and is not verified

The research environment could not reach any vendor site (korg.com,
teenage.engineering, arturia.com, doepfer.de, makenoisemusic.com, roland.com,
wikipedia.org, sweetwater.com, archive.org all failed: DNS or proxy 403).
**No vendor manual was read in full.** What was possible:

- A (strong, but not a vendor manual): firmware source on GitHub, read
  directly. Mutable Instruments Grids and Yarns (MIT licence header in the
  files) show real Eurorack clock conventions in code.
- B (medium): small open-source hobby projects on GitHub (code read directly)
  that talk to real hardware; they show how people drive real devices, not
  what the vendor specifies.
- C (weak, **unverified**): statements returned by a web search tool as
  summaries of pages (Wikipedia DIN sync, Arturia support, Korg manuals,
  muzines, etc.). The pages themselves were not fetched. They are cited by URL
  as leads, each marked "search summary only".

Every table row below carries its evidence class. Rows that are class C only
must be checked against the named manual before the preset names ship
(BUILD-PLAN row R4: "numeric ppqn is authoritative" remains the rule).

## 1. Summary table

| Device or family | Pulse rate | Level and polarity | Pulse width | Evidence |
|---|---|---|---|---|
| Roland DIN sync / Sync24 (TR-606, TR-808, TR-909, ...) | 24 ppqn | 5 V logic, rising edge = pulse; run line high = run, low = stop | not found in a primary source | C (search summary of https://en.wikipedia.org/wiki/DIN_sync and https://www.sweetwater.com/insync/din-sync/); B for the 5 V / high=run behaviour (below) |
| Korg legacy sync (DDM-110, DDM-220, KPR-77, KMS-30 selectable) | 48 ppqn | not found | not found | C only (search summary of https://www.muzines.co.uk/articles/korg-kms30/1800 and https://martianarts.net/korg-kms-30/) |
| Korg volca series SYNC IN | 2 ppqn in default (one pulse = two 16th steps) per several secondary descriptions; a 4 ppqn variant is also reported | accepts up to 20 V; SYNC OUT is 5 V | SYNC OUT 15 ms (Volca Drum manual, search snippet) | C (conflict between 2 and 4 ppqn: see section 4) |
| Teenage Engineering Pocket Operators SYNC IN | 2 ppqn (a square wave at BPM/60*2 Hz drives it) | audio-level pulses on a 3.5 mm jack, exact voltage not found | accepts 3-67 ms (TE statement quoted second-hand) | B for 2 ppqn (code); C for the width and level |
| Mutable Instruments Grids / Yarns clock | selectable 4, 8 or 24 ppqn (Grids); 24 ppqn base with divisions (Yarns) | 5 V-class gate | Yarns: 5 ms when undivided | A (code) |
| Arturia KeyStep (analog clock) | 5 selectable rates; "2 PPQ" described as the Korg standard | not found | not found | C only (search summary of https://support.arturia.com/hc/en-us/articles/4405741335954) |
| Make Noise (Tempi, Wogglebug) | not researched: no accessible source | | | none |
| Generic Eurorack clock (Doepfer A-100 family, etc.) | none standard; 1, 2, 4, 8, 16, 24, 48 ppqn are all seen | 5 V (A-100 gates are typically 0 V / +5 V or higher), rising edge, pulse >= a few ms | | no source retrieved: **unverified** |

## 2. Roland DIN sync / Sync24

### What the sources say

1. Rate: 24 pulses per quarter note (24 ppqn). Class C: search summary of
   https://en.wikipedia.org/wiki/DIN_sync ("5-volt timing pulses at a rate of
   24 PPQN"); the Sweetwater page
   (https://www.sweetwater.com/insync/sync-24/) titled "Sync 24" agrees with
   the name. Class B: https://github.com/itzaks/arduino-din-sync (a
   MIDI-to-DIN converter "TR-606 <3 Ableton Live", with start/stop) emits one
   2 ms high pulse (`delayMicroseconds(2000)`) per incoming MIDI clock, and
   MIDI clock is 24 ppqn, so the converter's output is 24 ppqn by
   construction.
2. Level: 5 V logic. Class C (Wikipedia summary above, "5-volt"). Class B: the
   same converter drives the pins directly from a 5 V Arduino with HIGH/LOW.
3. Run line: high = start/continue, low = stop. Class B: same repo
   (`HandleStart` and `HandleContinue` write HIGH, `HandleStop` writes LOW)
   and https://github.com/TravisThatcher/MIDI---DIN-SYNC (Start/Continue =
   HIGH, Stop = LOW, clock counter reset on stop). Class C: Wikipedia summary
   ("0 V stop, +5 V start/continue").
4. Polarity of the clock: positive pulse (low to high). Class B (both
   converters above idle low, pulse high). Caveat found in the same family:
   https://github.com/TravisThatcher/MIDI_TO_TRIGGER_SYNC README says the
   Roland CR-5000 uses negative-going triggers, so "polarity" is a real
   device-dependent option, which FS-09 already offers.
5. Pulse width: 2 ms (arduino-din-sync), 9 ms (MIDI---DIN-SYNC
   `CLOCK_LENGTH 9000` us, with a comment that tuning it fixes jitter), 5 ms
   (Volca SPG tool, below). No Roland figure found. FS-09's 5 ms default sits
   inside the range people use. **Unverified for real Roland inputs.**
   At 24 ppqn the pulse period is 20.8 ms at 120 BPM, 8.3 ms at 300 BPM.

### Pin numbers on the 5-pin DIN (the open question)

Class C only; **no primary source was retrieved, and sources conflict**.

- FS-09 (current spec) says: pin 1 clock, pin 2 ground, pin 3 run/stop.
- A web-search summary of the Wikipedia DIN sync article and related pages
  reports the opposite assignment for pins 1 and 3: pin 1 = start/stop
  (0 V stop, +5 V start/continue), pin 3 = clock (5 V pulses, 24 ppqn),
  pin 2 = ground; pin 4 optional reset (song mode), pin 5 optional fill-in
  (TR-808, TR-606), with a blog saying the TR-909 uses pin 5 as "Continue".
  (Search summary only; leads: https://en.wikipedia.org/wiki/DIN_sync,
  https://handwiki.org/wiki/DIN_sync,
  https://sunshine-jones.com/searching-for-the-perfect-sync/.)
- Hobby code gives no pin numbers: arduino-din-sync only names its
  Arduino pins (`PIN_TRIGGER 4`, `PIN_STARTSTOP 7`) and refers to a wiring
  picture hosted on http://gieskes.nl/ (not reachable).
- Also reported: DIN sync cables use pins 1, 2, 3 (not MIDI's 2, 4, 5), and the
  connector is the same 5-pin 180 degree DIN as MIDI, so MIDI cables may not
  carry the lines (class C).

Decision for FS-09: **do not hard-code or print the pin assignment in the
hardware doc as verified.** The two candidates differ by swapping pins 1 and
3. Make the hardware doc say "verify with a scope or the service manual of the
target unit" and name the check: with a Roland unit running as master, the
line that toggles at 24 ppqn is the clock. Best fix: get the Roland TR-606,
TR-808 or TR-909 owner or service manual page showing the DIN sync connector
(offline) and record the page number here. Cannot be closed without network
access to a vendor-manual source or a scope.

Level shifting note: a Pi GPIO is 3.3 V; 5 V logic is the stated level, so the
spec's 74AHCT125 buffer (class B-consistent with the 5 V figure) stays.

## 3. Korg 48 ppqn

- Search summary (class C, pages not fetched):
  https://www.muzines.co.uk/articles/korg-kms30/1800 and
  https://martianarts.net/korg-kms-30/ say the Korg KMS-30 MIDI synchroniser
  has three sync sockets each with a switch for 24 or 48 ppqn, and that Korg
  gear of the DDM-110, DDM-220 and KPR-77 era used 48 ppqn where Roland used
  24, so mixing them makes the Korg gear run at half speed.
- Models NOT confirmed by any retrieved source: SQ-10, Electribe family,
  Monotribe/Monologue/Minilogue (modern Korg analog sync I/O is the 3.5 mm
  "sync" jack with the volca-style pulse; the rate on each is unverified).
- Practical conclusion: FS-09's "48" preset name should be "Korg legacy (48)"
  and should not claim any specific modern Korg model. Unverified.

FS-09 implementation note unrelated to the rate: the spec generates the 48
ppqn mid-pulse by halving the previous tick period. At 300 BPM, 48 ppqn is
240 pulses per second, a 4.17 ms period, so the 40 percent clamp gives a
maximum width of about 1.67 ms; the 5 ms default is clamped automatically.

## 4. Korg volca (and "sync in")

What exists:

- Korg Volca Drum manual (search snippet of
  https://manualshive.com/en/manual/korg/volca-drum/1998639?page=3, class C):
  SYNC IN is a monaural 3.5 mm jack, 20 V maximum input level; SYNC OUT is a
  3.5 mm jack with 5 V output level and a 15 ms pulse. Same family manuals
  are also indexed at https://www.analoguehaven.com/korg/volca-kick/manual.pdf
  and https://analoguehaven.com/korg/volca-fm/manual.pdf (not fetched; these
  are the right places to confirm per-model sync text).
- Rate, two readings:
  - 2 ppqn: several secondary sources (Arturia support page "KeyStep ... to
    make it simple, you can consider it running at 2 PPQ ... the standard
    used by KORG"; a ModularGrid forum thread; an Arduino Slovakia article
    https://www.arduinoslovakia.eu/blog/2021/1/korg-volca-signal-sync-out
    "2 PPQN") all class C.
  - Class B, code that drives real Volcas:
    https://github.com/davemh/korg-volca-SPG (GPL-3.0; facts only) builds
    sync audio with `interval = beatDuration/2` and `pulseLength = 0.005`:
    two pulses per quarter note, 5 ms each, i.e. 2 ppqn.
    https://github.com/thomai-d/volcasync (Arduino module for syncing
    Volcas) has a "half-step mode (only pulses every second sync step)"
    where the base step is a 16th note (pulse length defined in 256ths of a
    1/16th), i.e. a normal output of 4 ppqn with a half-step option for 2 ppqn
    devices. Its README says "Volcas have huge problems with MIDI sync",
    "Volcas emit a single pulse upon startup" and the Keystep stops
    forwarding sync when stopped.
  - A search result reports a forum measurement suggesting a 4 ppqn pulse from
    a Volca Beats and a firmware update adding 4 ppqn ("1 pulse per step").
    Class C and conflicting.
- Conclusion for FS-09: the old "volca sync in = 1 pulse per step" idea in the
  task prompt is not settled; the most consistent reading is **2 ppqn =
  one pulse per two 16th steps** by default, with some models/firmware
  offering a faster option. The preset should be named "Korg volca (2)" and
  tell the user to check the model's SYNC IN section (class C; unverified per
  model).
- The 5 ms pulse default in FS-09 is consistent with the SPG tool and
  below the Volca Drum's own 15 ms output; the Volca's accepted minimum width
  was not found.

## 5. Teenage Engineering Pocket Operators

- 2 ppqn: Class B. https://github.com/orllewin/playdate_posync (Lua, a
  Playdate console making a "PO-Sync" signal): square wave synth played at
  frequency `(bpm/60) * 2` Hz, duty cycle 0.3, panned left. That is two cycles
  per beat = 2 ppqn. The README quotes a Soundtech page
  (https://www.soundtech.co.uk/music-retail/teenageengineering/news/pocket-operator-sync-modes,
  not fetched): "the sync signal worked correctly when the impulse time was
  between 3-67ms ... If they're too short, the impulse won't trigger ... if
  too long, the sequencer will start to skip steps". Second-hand, class C for
  the numbers.
- Level: PO sync in is an audio-level signal on a 3.5 mm jack (the Playdate
  drives it from a headphone-level output). No voltage given in anything
  retrieved. **Unverified**, so the 5 V buffered output is expected to work but
  amplitude tolerance should be tested on a PO (a TS jack with a divider
  option is cheap insurance).
- Mode note (class C from the same page title): PO "sync modes" exist (sync
  in/out combinations); a 5 ms pulse at 2 ppqn (500 ms period at 120 BPM)
  is inside 3-67 ms.
- Polarity for PO: not found; positive pulses used by the Playdate patch.

## 6. Eurorack clock conventions

- Mutable Instruments Grids (https://github.com/pichenettes/eurorack, commit
  f4cfa76; source files carry an MIT permission notice):
  `grids/pattern_generator.h` defines `CLOCK_RESOLUTION_4_PPQN`,
  `_8_PPQN`, `_24_PPQN`, comments "kPulsesPerStep = 3; // 24 ppqn; 8 steps per
  quarter note", default `CLOCK_RESOLUTION_24_PPQN` in `clock.h`. Class A.
- Mutable Instruments Yarns (`yarns/multi.cc`): its clock output comment says
  "no division is used ... we output 5ms pulses" (code `clock_pulse_counter_ =
  40` on an 8 kHz refresh, 40 / 8000 = 5 ms) and otherwise a 50 percent duty
  cycle; the clock base is the MIDI 24 ppqn clock and the user picks output
  divisions. Class A. This supports FS-09's 5 ms default for Eurorack.
- Rising edge: both firmwares treat the rising edge as the trigger (standard
  for gate/trigger inputs; the specific Yarns/Grids input stage was not
  inspected). Pulse width and 5 V: widely used practice, no vendor
  statement retrieved for Doepfer, Make Noise or Arturia. **Unverified.**
- Arturia KeyStep: "The KEYSTEP supports 5 different analog clock rates"
  (title of https://support.arturia.com/hc/en-us/articles/4405741335954);
  contents not retrieved. A GitHub project (volcasync) reports the KeyStep
  "stops forwarding syncs when sequencing is stopped" (class B). Defaults not
  found.
- Make Noise: nothing retrieved. Their clocking manual
  (https://www.makenoisemusic.com/content/manuals/ModularClockingManual.pdf)
  failed to load. **Unverified.**

## 7. Recommendations for FS-09

1. Keep `ppqn` numeric choices 1, 2, 4, 24, 48 (all are justified: 24 Roland,
   48 Korg legacy, 4 and 2 for Volca/PO variants and Eurorack, 1 for
   quarter-note triggers). Optionally add 8 (Grids offers it) later.
2. Preset names. Change "16th pulse (4)" and "8th pulse (2)" to neutral
   labels plus a hint, and keep vendor names out until the manual check:
   - "Roland DIN sync (24)" (supported by classes B and C).
   - "Korg legacy (48)" (class C).
   - "Korg volca / Pocket Operator (2)" (2 ppqn: class B for PO and for the
     Volca SPG tool, class C for Korg manuals; flag "check model").
   - "16th (4)" and "Quarter (1)" stay generic.
3. Pulse width default 5 ms stays: it matches Yarns (class A), the Volca SPG
   tool (B) and is inside the PO's quoted 3-67 ms range (C). Document that
   DIN sync inputs of some Rolands may need longer pulses (hobby converters use
   2 to 9 ms) and make the field user editable (it already is).
4. Hardware doc must not assert pin 1 = clock until verified (section 2). Add
   a "verified against: <manual page>" line to `pulse-out.md` and block the
   BOM release on it.
5. Add a test option for a polarity-low (active-low) Roland CR-5000 style
   input; FS-09 already has `polarity`.
6. Tests worth adding: PU-T3 extended with 24 ppqn at 300 BPM (8.3 ms period)
   confirming width clamp at 40 percent = 3.3 ms; 5 ms default is clamped.

## 8. Open items (D0 cannot close these offline)

- O1 Pin 1/pin 3 assignment on 5-pin DIN (conflict, section 2). Needs a
  Roland manual page or a scope.
- O2 Per-model Korg ppqn (SQ-10, Electribe, Monotribe) and the existence of a
  4 ppqn mode on Volca firmware.
- O3 PO SYNC IN voltage and polarity tolerance.
- O4 Arturia KeyStep five clock rates and defaults; Make Noise defaults;
  Doepfer A-100 gate level and minimum pulse width.
- O5 Whether Roland inputs need a minimum pulse width above 2 ms.

## Sources

Read directly (git clone / raw.githubusercontent.com, 2026-11-04):
- https://github.com/pichenettes/eurorack (grids, yarns) commit f4cfa76
- https://github.com/itzaks/arduino-din-sync
- https://github.com/TravisThatcher/MIDI---DIN-SYNC
- https://github.com/TravisThatcher/MIDI_TO_TRIGGER_SYNC
- https://github.com/equaliser/Sync24-Clock-Divider
- https://github.com/davemh/korg-volca-SPG (GPL-3.0)
- https://github.com/thomai-d/volcasync
- https://github.com/orllewin/playdate_posync
- https://github.com/nuclearsugar/VolcaClockPlayground (MIT)

Search-tool summaries only, page not fetched (treat as leads):
- https://en.wikipedia.org/wiki/DIN_sync
- https://www.sweetwater.com/insync/din-sync/ and /sync-24/
- https://handwiki.org/wiki/DIN_sync
- https://sunshine-jones.com/searching-for-the-perfect-sync/
- https://www.muzines.co.uk/articles/korg-kms30/1800
- https://martianarts.net/korg-kms-30/
- https://manualshive.com/en/manual/korg/volca-drum/1998639?page=3
- https://www.analoguehaven.com/korg/volca-kick/manual.pdf
- https://support.arturia.com/hc/en-us/articles/4405741335954
- https://www.arduinoslovakia.eu/blog/2021/1/korg-volca-signal-sync-out
- https://www.soundtech.co.uk/music-retail/teenageengineering/news/pocket-operator-sync-modes
