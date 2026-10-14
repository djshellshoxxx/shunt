# Build plan for features FS-01 to FS-09

Goal: ship nine features with few bugs and almost no merge conflicts. The
whole approach is: **build the seams once, then add files, not edits.**

## 1. Waves and order

| Wave | PRs | Why this order |
|---|---|---|
| W0 | **PR 0** FS-00 extension points; **PR T0** FS-TEST (CI jobs, simulator registry and scenarios, fixture builder) | everything else depends on them; they are the only PRs that touch shared files. PR 0 and T0 touch different files and can run in parallel |
| W1 | FS-01 on-air, FS-04 MIDI learn, FS-05 replay | independent of each other; FS-01 and FS-04 are the most useful to ship early; FS-05 gives the test fixtures that W2 and W3 reuse |
| W2 | FS-02 key clash (needs MetaStore from PR 0, optionally FS-01), FS-06 tap/nudge (registers actions; better after FS-04), FS-07 profiles (after FS-04 so `profile.*` actions are bindable) | each only adds its own files |
| W3 | FS-08 set report (after FS-05 for its fixture and FS-01 for mute data), FS-09 pulse (after FS-01), FS-03 phrase (spike first, hardware needed) | most coupled or riskiest |

Spikes that can start on day 1 in parallel with W0: K0 (decode status key
bytes, needs a CDJ-3000 capture), P0 (verify PSSI format on three `.EXT`
files, no hardware), N0 (are phrase packets on the network, needs a
CDJ-3000), D0 (verify DIN sync pulse rates in manuals).

Dependency graph:

```
PR0 --+--> FS-01 --+--> FS-09
      |            +--> FS-08 (optional data)
      +--> FS-04 --+--> FS-06 (bindable)  +--> FS-07
      +--> FS-05 ----> FS-08 (fixture)
      +--> FS-02
      +--> FS-03 (after P0)
T0  ---> all (scenarios, fixtures, CI)
```

## 2. Effort

PR 0: 3 days. T0: 3 days. FS-01 3 to 4. FS-02 2 to 3. FS-03 5 to 7.
FS-04 3 to 4. FS-05 4. FS-06 3. FS-07 3. FS-08 4. FS-09 4 incl. hardware.
Single developer total about 38 to 44 days; with three working in parallel
after W0 about 5 weeks end to end.

## 3. Branches, PRs and merging

- One branch and one PR per feature: `feat/<id>` (for this repo's agent
  sessions the harness branch names apply; the rules below are the same).
- Always branch from the latest `main` after PR 0 and T0 have merged. Never
  stack a feature PR on another feature PR; if one needs another, wait for the
  merge (the graph above is the order).
- Rebase onto `main` (or merge `main` in, for a branch others use) at least
  daily and before asking for review; resolve conflicts immediately while
  they are small. Do not rewrite history of a branch another person pushed to.
- Squash merge. PR title `FS-0N: name`. Keep a PR under about 800 changed
  lines excluding goldens and fixtures; if larger, split into
  "logic + tests" and "wiring + UI" PRs (logic first).
- CI required: linux, aarch64, sanitize-asan, sanitize-tsan, ui-smoke.
- A feature PR may touch only: its own new files (section 5 table), its own
  test and golden files, its own spec, and one line in the plan and README.

## 4. Frozen files (after PR 0 and T0)

These change only through a dedicated PR labelled `core`, reviewed by the
maintainer. Feature PRs must not edit them:

`app/src/Core.cpp`, `app/include/shunt/app/Core.h`, `app/src/Api.cpp`,
`app/src/Settings.cpp`, `app/include/shunt/app/Settings.h`,
`app/src/Http.cpp`, `CMakeLists.txt`, `cmake/*.cmake`,
`ui/index.template.html`, `ui/js/00-core.js` to `40-diagnostics.js`,
`ui/css/00-base.css`, `tests/TestFramework.h`, `tests/FixtureBuilder.h`,
`tools/shuntsim/main.cpp`, `.github/workflows/ci.yml`, `docs/specs/RS-07-*.md`.

If a feature genuinely needs a change in a frozen file (e.g. FS-01 needs
`MidiClockGenerator::setMuted`): `out/src/Midi.cpp` and `OutputManager.cpp`
are NOT frozen but are shared, so the rule is: the owning feature makes that
edit in the **first commit** of its PR as a small, separately reviewable
change, and the shared-file ownership table below says who edits what.

Shared non-frozen files and their single owner during the project:

| File | Owner | Others |
|---|---|---|
| `out/src/Midi.cpp`, `Midi.h` | FS-01 (mute) | FS-04 adds `MidiIn.*` as new files; FS-09 consumes the stream only |
| `out/src/OutputManager.cpp` | FS-01 (mute flag), then FS-09 (`dispatchMidiMessages`) in W3 | none in W1/W2 |
| `out/src/Osc.cpp` | FS-01 (any/mute/qlab cues) first; later features add messages through a new `OscExtras` hook registered by the feature, defined by FS-01's PR | FS-02, FS-03 use the hook, not edits |
| `tools/shuntsim/scenarios/` | T0 creates all scenarios | features may fix a bug in their own scenario only |

To avoid FS-02/FS-03 editing `Osc.cpp`, FS-01 delivers
`OscOutput::addExtra(std::function<void(const Msg&)>)` / a public
`OutputManager::sendOsc(address, args)` that features call; this is part of
FS-01's acceptance and is documented in its spec deviations section.

## 5. File ownership per feature (what each PR adds)

| FS | New files |
|---|---|
| 01 | `app/src/features/OnAir.{h,cpp}`, `ui/js/60-onair.js`, `ui/css/60-onair.css`, `tests/features/test_onair.cpp` |
| 02 | `app/src/features/KeyMath.{h,cpp}`, `KeyClash.cpp`, `ui/js/61-keyclash.js`, `tests/features/test_keyclash.cpp`, `tests/golden/key_levels.txt` |
| 03 | `log/src/AnlzPssi.cpp`+h, `app/src/features/Phrase.cpp`, `tools/shunt_anlz/main.cpp`, `ui/js/62-phrase.js`, `tests/features/test_phrase.cpp`, `tests/fixtures/anlz/*`, `docs/research/04-pssi-format.md` |
| 04 | `out/src/MidiIn.cpp`+h, `app/src/features/MidiLearn.cpp`, `ui/js/63-midilearn.js`, `tests/features/test_midilearn.cpp` |
| 05 | `app/src/features/Replay.cpp`, `ui/js/64-replay.js`, `tests/features/test_replay.cpp` |
| 06 | `app/src/features/Nudge.cpp`, `ui/js/65-nudge.js`, `tests/features/test_nudge.cpp` |
| 07 | `app/src/features/Profiles.cpp`, `ui/js/66-profiles.js`, `tests/features/test_profiles.cpp` |
| 08 | `app/src/features/SetStats.{h,cpp}`, `Report.cpp`, `ui/js/67-report.js`, `tests/features/test_report.cpp`, `tests/golden/report_small.html` |
| 09 | `out/src/PulseGenerator.cpp`+h, `out/src/GpioCdev.cpp`, `app/src/features/Pulse.cpp`, `ui/js/68-pulse.js`, `tests/features/test_pulse.cpp`, `docs/hardware/pulse-out.md`, `pulse-jitter-test.md` |

Everything is picked up by globs, so no CMake edits.

## 6. Implementation method per feature (same every time)

1. **Spec check (0.5 day):** read the spec, list every ambiguity as a spec
   PR comment, fix the spec first. Spike items (K0, P0, N0, D0) are separate
   tasks merged as research notes before dependent code.
2. **Pure logic first:** write the pure class and its tests with synthetic
   time; no threads, no sockets, no JSON. Merge-ready on its own.
3. **Feature class:** wire to `IFeature` with settings validation and status;
   tests for shapes, disabled-is-inert, enabled behaviour with the simulator
   scenario.
4. **HTTP routes** with validation tests (sizes, types, bad ids).
5. **UI module** using `h()`; add the page and widget; screenshots at 1440 and
   360 px; run `ui-smoke`.
6. **Integration run** against the scenario with outputs captured; check the
   acceptance bullets in the spec; paste evidence in the PR.
7. **Sanitizers locally** (`cmake -DSHUNT_SANITIZE=thread`) before pushing.
8. **Self review** against the Definition of done in FS-TEST section 5.

## 7. Bug-prevention rules

- The locking contract in FS-00 section 1 is law; reviewers check every new
  `handle()` and callback against it.
- Time is an input, never `now()` inside logic classes.
- Every external number is clamped at the boundary (settings, HTTP, MIDI,
  OSC, files) and the clamp has a test.
- Features never throw out of callbacks; the host wraps each call in
  `try/catch`, logs, and disables the feature after 5 exceptions in a minute
  (status shows `error`). Test with a dummy throwing feature.
- No global mutable state outside the registry singleton.
- Default off, one switch (`features.<id>.enabled`), proven inert.
- Persisted formats carry `schema` numbers and tolerate unknown keys.
- Every user-visible string from the network is rendered with `textContent`.

## 8. Release and rollout

Tag `v0.2.0` after W1, `v0.3.0` after W2, `v0.4.0` after W3. Each release:
changelog from PR titles, `ui-smoke` screenshots attached, compatibility
badges reviewed. Features ship disabled; enabling is per feature in the UI
(Outputs page) so a field problem is solved by switching one thing off.

## 9. Risk and open-question register

| # | Item | Impact | Owner | Resolution |
|---|---|---|---|---|
| R1 | Status key byte encoding unknown | FS-02 falls back to XML keys only | K0 | capture and decode; feature works without |
| R2 | PSSI offsets, XOR mask unverified | FS-03 parser | P0 | verify on 3 real files before code |
| R3 | Phrase data on the network unknown | FS-03 source B | N0 | needs CDJ-3000 captures; source A ships alone |
| R4 | DIN sync vendor pulse rates | FS-09 presets could mislead | D0 | check manuals; numeric ppqn is authoritative |
| R5 | Mixer channel to deck mapping not identity | FS-01 wrong mute | design | `channelMap` setting and UI editor |
| R6 | Master tempo (key lock) flag not parsed | FS-02 pitch shift | design | `assumeKeyLock` default true; revisit with captures |
| R7 | GPIO timing under load on Pi | FS-09 jitter | hardware test | RT priority, spin-wait last 150 us, measured acceptance |
| R8 | Locking regressions as features multiply | crashes in a booth | FS-00, TSan | lock contract, TSan gate, dummy-feature stress test |
| R9 | Merge conflicts in UI and Core | slow merges | PR 0 | frozen files, glob builds, numbered UI modules |
| R10 | Replay accidentally driving real outputs | live gear triggered | FS-05 | outputs off by default, banner, tests |
| R11 | Web UI has no authentication | anyone on the LAN can change settings | later | `bind` setting now; token auth proposal after W3 (touches frozen files, own PR) |
| R12 | Large uploads (XML 64 MB, pcapng 256 MB) held in memory | memory spikes on Pi | FS-05 | per-route limit, optional upload, streaming later |

## 10. Tracking

One GitHub issue per spec section "Tests" and "Acceptance" is not needed:
the spec checklists are the issue. Add a milestone per wave. Keep
`docs/PROJECT-PLAN.md` ticks one line per feature; update only in the feature
PR that implements it.
