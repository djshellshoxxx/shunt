# ES-03: Clock engine and timing (detailed engineering spec)

Status: draft 0.1, normative. Research: `docs/research/03-timing-appliance-integrations.md`
sections 1 and 8; `docs/research/01-pro-dj-link-protocol.md` sections 5 and 7.

The clock engine turns the master deck's beat and status events into one
smooth, predictable beat timeline, and the outputs (ES-04) consume it. It
is a plain C++17 library (`shunt_clock`) with no sockets and no JUCE, so the
synthetic-jitter tests run in milliseconds.

---

## 1. Inputs and outputs

Inputs: the event stream from ES-01 section 7, plus user settings
(latency offset, bar offset, mode, profile).

Output: a `Timeline` snapshot, published through a seqlock on every update:

```cpp
struct Timeline {
    double   bpm;              // filtered effective tempo
    int64_t  beatOriginNs;     // monotonic time of the most recent predicted beat
    int64_t  beatIndex;        // running beat counter since lock
    int      beatInBar;        // 1..4 at beatOriginNs, after bar offset
    bool     barKnown;         // false for mixer master, Opus Quad, beat-only mode
    bool     playing;          // master is playing
    float    confidence;       // 0..1
    uint32_t sequence;         // increments per update
    uint32_t resetSequence;    // increments on every hard re-phase (outputs jump once)
    uint8_t  masterDevice;
};
```

`timeOfBeat(k) = beatOriginNs + (k - beatIndex) * 60e9 / bpm`.

## 2. Per-player state

One `PlayerTrack` per device, all kept warm so a handoff starts from a
converged filter:

```
lastBeatRecvNs, lastBeatInBar, reportedBpm (bpm100 * pitch1 from status,
fallback beat packet), beatNumber (status), playState, onAir,
filter {phaseNs, periodNs, alpha, beta, boostBeatsLeft},
residualHistory[16], outlierRun, hitCount
```

Only the master's track drives the `Timeline`; the mixer's beats are used
only in MixerMaster state.

## 3. Estimator

Alpha-beta filter on (phase, period) in beat-index space, per research 03
section 8.

On each accepted master beat with receive time `t` (already minus
`latencyOffsetNs`, default 1 ms, range -20 to +50 ms):

```
predicted = phase + period
r = t - predicted                       // innovation
if |r| > max(25 ms, 3 * MAD(residualHistory)):  outlier
    outlierRun++ ; if outlierRun >= 2 and same sign: RESET(t)
    else: phase = predicted (coast), return
outlierRun = 0
phase  = predicted + alpha * r
periodMeasured = t - lastAcceptedBeatTime
periodReported = 60e9 / reportedBpm
period = 0.9 * periodReported + 0.1 * (period + beta * (periodMeasured - period))
```

Gains: Stage profile alpha 0.3, beta 0.05; Rehearsal alpha 0.5, beta 0.1.
After a pitch-field change in status (Pitch_1 moves more than 0.05 percent)
or a nudge (10 ms < |r| < outlier gate), set `boostBeatsLeft = 8` with alpha
0.7.

The reported tempo dominates the period because the player's own number is
nearly noise-free; arrival timing carries phase. This removes pitch-bend
lag without making the clock twitchy.

Duplicate guard: a beat arriving less than period / 5 after the previous
accepted beat is dropped (TimeFinder rule).

## 4. Reset (hard re-phase)

`RESET(t)`: `phase = t`, `period = periodReported`, residual history cleared,
`resetSequence++`. Triggered by:

- master change (ES-01 `MasterChanged`), using the new master's warm
  track;
- two consecutive same-sign outliers;
- status beat number jumping by more than 1 beat beyond what elapsed time
  predicts (hot cue, un-quantised loop);
- pause to play transition (after two beats are seen).

Rate limit: at most one reset per 4 beats unless a master change.

## 5. Bar phase

`beatInBar = ((packetBeatInBar - 1 + userBarOffset) mod 4) + 1` from the
master's beat packet. `barKnown = false` when: master is a mixer, the
device is Opus Quad, beat-only mode is selected, or the master's track type
is not rekordbox. Quantised loops keep beat phase; bar phase is re-derived
from the packet on every beat so loop re-entry is handled implicitly.

Bar offset memory: keyed by (rekordbox id, source slot), stored when the
user changes the offset while that track is loaded, reapplied on next load.

## 6. State machine

```
Idle ──first master beat──> Locking ──2 beats──> Locked
Locked ──no master beat for 1.5 periods and status says playing──> Coasting (dead-reckon)
Coasting ──beat──> Locked ; Coasting ──status stopped or 1 s──> Paused
Locked ──status stopped──> Paused (hold bpm, phase frozen, confidence decays)
Paused ──beats resume──> Locking (RESET after 2 beats)
any ──MasterChanged──> Locking on the new master (RESET)
NoMaster with mixer ──> MixerMaster (tempo from 0x29, phase from mixer beats, barKnown=false)
```

Confidence: `0.5 * hitRate(last 8) + 0.3 * exp(-(rmsResidual / 8 ms)^2) +
0.2 * (state == Locked)`; decays with 2 s time constant in Paused and
Coasting.

## 7. Publishing policy (consumed by ES-04)

- Tempo: outputs apply a new tempo only when `|bpm - lastPublished| > 0.02`
  and at least 1 s since the last publish, or on reset.
- Phase: outputs correct phase softly when the error exceeds 1/60 beat for
  two consecutive beats, hard on `resetSequence` change.
- Pause behaviour per output: Link start/stop state if enabled; MIDI clock
  "keep running" (default) or Stop; OSC `/transport 0`.

## 8. Test requirements (synthetic, no network)

- C-T1. Steady state, Gaussian arrival jitter sigma 1 ms: phase RMS error
  under 1 ms, tempo within 0.01 BPM after 8 beats.
- C-T2. Sigma 10 ms: phase RMS under 4 ms.
- C-T3. One 100 ms outlier: phase estimate moves under 1 ms; no reset.
- C-T4. Master handoff 128 to 130 BPM with 180 degree phase difference:
  exactly one reset; tempo at 130 within 0.05 BPM by the 4th beat.
- C-T5. Pitch slider ramp +2 percent over 8 beats: tracked within 0.2 BPM
  throughout; no reset.
- C-T6. Nudge (+15 ms for 4 beats then back): phase error under 6 ms peak;
  no reset.
- C-T7. 4-beat quantised loop and hot cue mid-bar: loop produces no reset
  and bar phase correct; hot cue produces one reset.
- C-T8. Pause 5 s then resume at a new phase: Paused state entered within
  250 ms of the status stop; one reset on resume.
- C-T9. 3 percent packet loss: no resets; beat index never skips.
- C-T10. Publishing policy: tempo publishes never exceed 1 per second
  except on reset.
