# FS-TEST: Fixtures, simulator scenarios, CI additions and definition of done

Status: draft 0.1. Applies to FS-00 to FS-09. Delivered as PR T0 (before the
feature wave) so no feature edits shared test infrastructure.

## 1. CI additions (T0)

| Job | What | Why |
|---|---|---|
| `sanitize-asan` | build with `-fsanitize=address,undefined`, run `ShuntTests` | memory and UB bugs in parsers and JSON |
| `sanitize-tsan` | build with `-fsanitize=thread`, run `ShuntTests` (excluding the simulator process test) | lock-order and race bugs from FS-00 section 1 |
| `ui-smoke` | build, start `shunt_headless` on loopback plus `shuntsim`, run `tools/ui_smoke.js` (Playwright, bundled Chromium) | catches broken JS, missing pages, console errors, 404s |
| existing `linux`, `aarch64` | unchanged | |

`tools/ui_smoke.js`: visits every nav page, asserts no `pageerror`, no
`console.error`, no failed request, that each registered page id mounts,
that `/api/status` contains `features` keys for every enabled feature, and
takes screenshots saved as CI artifacts. It must run in under 90 s.

All jobs required for merge. TSan is the gate for the locking contract.

## 2. Simulator scenarios (T0, `tools/shuntsim/scenarios/*.cpp`)

T0 turns `shuntsim/main.cpp` into a registry (`REGISTER_SCENARIO(name, fn)`),
moves the existing scenarios without changing behaviour, and adds the
following so features never touch the simulator again. Each scenario is
deterministic given `--seed`, documents its timeline in a header comment, and
prints `scenario <name> t=<s> <event>` lines with `--verbose` so tests can
assert on timing.

| Scenario | Timeline (seconds) | Used by |
|---|---|---|
| `onair` | 0 deck 1 master and on air (mixer on-air packet ch1); 10 crossfade to deck 2 (ch1 off, ch2 on, deck 2 master at 12); 30 all channels off; 36 ch2 on again; 45 end | FS-01, FS-08 |
| `onair-nomixer` | as above using only player F-byte bits (no 0x03 packets) | FS-01 |
| `key` | decks load tracks id 2001 (8A) and 2002 (3B): `--key-bytes` writes given 3 bytes at 0x15c on CDJ-3000 decks so K0 can be replayed; deck 1 on air, deck 2 cued; at 20 s deck 2 loads id 2003 (9A) | FS-02 |
| `phrase` | deck 1 plays a track whose beat number advances from 1; at 20 s hot cue jumps back 32 beats; at 40 s loop of 4 beats for 8 s | FS-03, FS-05 |
| `wrongbar` | master reports `beatInBar` shifted by +2 relative to the true downbeat marker (`--bar-shift N`) | FS-06 |
| `longset` | 20 minutes, 6 tracks, two master handoffs, light jitter and 1 percent loss | FS-05, FS-08 |

Also new flags: `--record out.pcapng` (write exactly what is sent, using
`PcapngWriter`) so fixtures are made by the simulator, never by hand;
`--time-scale X` to run `longset` fast in CI (default 1).

## 3. Fixtures and golden files

- No binary fixtures are committed except real-world ANLZ samples for FS-03
  (small, anonymised, 3 files, documented provenance, user's own tracks).
- pcapng fixtures are generated in tests by `tests/FixtureBuilder.h`
  (T0): `buildCapture({scenario params})` using the Packets builders and
  `PcapngWriter` into memory/temp. Deterministic seed.
- Golden text files live in `tests/golden/`. A test that compares against a
  golden prints a unified diff on failure; `UPDATE_GOLDEN=1` rewrites; the PR
  must show the golden diff and say why.
- `tests/TestFramework.h` gains `CHECK_STR`, `TEMP_DIR()` (auto-removed),
  `WITH_TIMEOUT(ms, body)` (kills hung tests) and `CHECK_JSON(expr, path, value)`
  (T0), so features do not each reinvent them.

## 4. Test conventions

- Test ids: `<feature prefix>-T<n>` in the `TEST_CASE` name (OA, K, PH, ML, R, N,
  PR, RP, PU, O for outputs).
- Pure logic in pure classes, tested with synthetic time. Threads and sockets
  only in a few integration tests with `WITH_TIMEOUT`.
- Every public JSON shape has a test asserting key set and types.
- Every settings field has a validation test (clamp, wrong type, missing).
- Any parser has a fuzz-ish test: truncations at every byte and random
  mutation for 2000 iterations with a fixed seed (no crash, no hang).

## 5. Definition of done (every feature PR)

1. Spec status set to `implemented`, deviations listed at the end of the spec.
2. All spec tests present and named by id; all CI jobs green including TSan,
   ASan/UBSan and ui-smoke.
3. Feature disabled by default (except where the spec says otherwise) and a
   test proves status, outputs and behaviour are unchanged when disabled.
4. No edits to the frozen files list in BUILD-PLAN section 4.
5. UI: keyboard operable, 14 px minimum text, works at 360 px width and
   1440 px; screenshot at both widths attached to the PR; no `innerHTML` with
   data from the network or settings (use `textContent` / `h()`).
6. Security: every new HTTP route validates input size and types, ids are
   whitelisted, no path built from user input except through a slug check.
7. Performance: the net-thread callbacks allocate nothing in steady state
   (checked by a counting allocator test for hot paths) and each feature's
   `onTick` costs under 20 microseconds on the CI runner (test with a budget
   and generous 10x CI slack).
8. Docs: one paragraph in the README feature list, plan checkbox ticked on
   its own line, API routes listed in the spec (not in RS-07).
9. PR description lists what was verified by hand and what needs hardware.

## 6. Performance budgets (global)

Status JSON build under 1 ms with 6 devices and all features on; WebSocket
frame rate 10 Hz; output thread wake jitter p99 under 0.3 ms on the CI runner
(informational, not gating); memory growth over a 20 minute `longset` replay
under 5 MB.
