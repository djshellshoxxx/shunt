# FS-05: Replay a capture

Status: implemented. Depends on FS-00 (section 7 is the seam as built). Feature id
`replay` (default off). UI `64-replay.js`, `64-replay.css`.

## Goal

Run a recorded pcapng through the whole stack (engine, tracklist, features,
UI) for demos, soundchecks without a booth, bug reports and regression tests.
`shunt_cli --replay` already does a console-only version.

## Behaviour

- **Files:** list pcapng files in the captures folder (setting
  `features.replay.capturesDir`, default `$HOME/.local/share/shunt/captures`,
  see Deviations). For each: name, size, modified time, and (lazily, cached by
  size and mtime) datagram count, duration, devices seen (number and name from
  keep-alives).
- **Start:** pick a file, speed (0.25, 0.5, 1, 2, 4, 8), loop on/off,
  "send outputs" (default OFF: replay must not drive a real lighting rig or
  hardware sequencer unless the user opts in; outputs need speed 1).
- **Isolation:** while replaying, the live network stack is stopped (no
  sockets, no claim, no keep-alives sent) and the UI shows a persistent amber
  banner "REPLAY name  mm:ss / mm:ss  x1  outputs off|ON" with Stop. A
  recording request takes effect when the replay ends (FS-00 deviation 11).
  The real tracklist session is parked by the host and a separate in-memory
  session is used; replay rows are never written to `<data>/tracklists/` and
  are discarded on stop (export during replay is allowed from the replay
  session).
- **Controls:** pause, resume, seek (to second `t`: new engine and session,
  then fast-forward by feeding the datagrams from `t - 30 s` up to `t` with
  synthetic time, then continue), speed and loop changes while running, stop
  (restore live stack and parked session).
- **Loop and end:** at end of file with loop: reset engine, device table and
  session, continue from 0; without loop: hold last state and show "ended"
  with Restart (seek 0) and Stop.
- **Engine reset points:** start, seek, loop wrap (`FeatureHost::resetEngine(true)`:
  new `ClockEngine`, `DeviceTable` and replay session) so the timeline never
  sees a time jump. Pause and resume do not reset: the engine clock simply
  stands still while paused.
- **Time mapping:** the replay position `p` (capture ns since the first kept
  datagram) advances as `anchorPos + (now - anchorNow) * speed`. Datagram `d`
  is fed once `p >= d.rel` with `recvNs = engineOffset + d.rel`: the engine
  always sees capture-time spacing (so it locks at any speed) and
  `engineOffset = now - target` is set at each reset point, so at speed 1
  `recvNs` tracks the monotonic clock. Wall clock for the tracklist is the
  datagram's own pcapng timestamp (`captureStartWall + d.rel`).
- Only UDP to ports 50000..50002 is replayed (as `shunt_cli --replay`);
  other traffic in a capture is counted as `ignored`.

## API (`/api/f/replay/`)

| Route | Body | Result |
|---|---|---|
| `GET status` | | status object (below) |
| `GET files` | | `{dir, maxUploadBytes, files:[{name, size, mtimeMs, info}]}`; `info` is `null` until scanned |
| `GET info?file=x.pcapng` | | `{name, info:{datagrams, ignored, durationS, devices:[{number, name}], error}}`; scans and caches |
| `POST start` | `{file, speed?, loop?, outputs?}` | status; 400 bad name or types or outputs with speed != 1, 404 missing, 409 feature disabled, 422 invalid capture, 503 core not running; while running it switches file |
| `POST pause`, `resume` | | status; 409 when nothing runs |
| `POST seek` | `{t}` | seconds, clamped to `[0, duration]`; 400 non-number |
| `POST set` | `{speed?, loop?}` | speed snaps to the allowed list; 400 for speed != 1 while outputs are on |
| `POST stop` | | `{ok:true}`, idempotent |
| `POST upload?name=x.pcapng[&replace=1]` | raw pcapng bytes | 201 `{ok, name, size, packets}`; 400 name, 409 exists or feature disabled, 413 over 256 MiB, 422 invalid capture |

Request bodies other than upload are limited to 4 KiB (413). The name is
percent-decoded, then every byte outside `[A-Za-z0-9._-]` becomes `_`, leading
dots are dropped, it must end `.pcapng` (max 128 chars); start and info take
only names that already pass that whitelist. Uploads are written to a hidden
temp file, fsynced and renamed into place.

Status (`status.features.replay`, plus host keys `enabled`, `error`):
`{active, file, positionS, durationS, speed, paused, loop, ended, outputs,
loops, datagrams, fed, lastError}`.

Settings `features.replay`: `enabled` (bool, default false), `capturesDir`
(absolute path, < 1024 chars; anything else falls back to the default).
Disabling the feature stops a running replay.

## Design

- `app/src/features/Replay.h` (pure, no threads or clocks): `checkPcapng` /
  `checkPcapngFile` (structure check: section header first with the
  little-endian byte-order magic; every block length >= 12, a multiple of 4,
  inside the file, at most 16 MiB and repeated at the block end; EPB captured
  length inside its block; clear messages for classic pcap, big-endian and
  truncated files), `Capture` (compact: one payload arena plus 24-byte
  index entries; caps 500 MB file, 5 million datagrams), `loadCapture`
  (check, then `net::PcapngReader`, then `buildCapture`), `validName` /
  `sanitizeName`, `snapSpeed`, and `ReplayPlayer` (`load`, `pause`, `resume`,
  `seek`, `setSpeed`, `setLoop`, `step(nowNs, sink, budget)`).
- `ReplayFeature` (`Replay.cpp`) is the `IFeature` and the `ReplayHooks`.
  `step(nowNs)` (net thread, Core lock held, every ~5 ms) runs
  `ReplayPlayer::step` with a sink that calls `FeatureHost::feed` (with the
  capture's wall time) and `FeatureHost::resetEngine(true)`, feeding at most
  5000 datagrams per pass to bound the lock hold, then
  `setOutputsEnabled(outputs)`.
- Seam as built (FS-00 section 7): the host, not the hooks, supplies `feed`,
  `resetEngine` and `setOutputsEnabled`; `enterReplay(this)` parks the live
  engine, devices and session and turns outputs off; `leaveReplay()` restores
  them and rebuilds the live stack. The net loop calls `step` instead of
  polling the stack.
- Locks: `ctlMu_` serialises HTTP control operations and is never taken under
  the Core lock, so `handle()` may call `enterReplay`/`leaveReplay` while
  holding it. `mu_` guards player state; it is taken in `step`/`status`
  (Core lock held, order Core then `mu_`) and in `handle()` only around local
  state, never while calling the host. `cacheMu_` (file info) is a leaf.
- Parsing happens on the HTTP thread before the swap (can take seconds for big
  files, which stalls that single-threaded server meanwhile); the swap itself
  is a pointer move under `mu_`.
- `HttpServer` has per-route body limits (core change made for this
  feature): `setMaxBody(bytes)`, `setMaxBody(prefix, bytes)` (longest prefix
  wins), `maxBodyFor(path)`; defaults 64 MiB and 256 MiB for
  `/api/f/replay/upload`. A large body is moved, not copied, into the request
  and the buffer is reserved once from Content-Length (peak memory about one
  body).

## UI

Page `#/replay` (nav only while the feature is enabled): captures table
(name, size, duration, devices, Replay button; durations and devices filled
lazily from `info`), start options (speed, loop, "Send outputs" with a
warning; ticking it pins speed 1), upload button (asks before replacing),
transport card while active (play/pause, scrub slider, position, speed,
loop, Restart when ended, Stop). Banner widget in the shell slot `banner`.
At 360 px the size and devices columns are hidden.

## Tests (`tests/features/test_replay.cpp`)

- R-T1 fixture (FixtureBuilder, 20 s, deck 1 master 128 BPM, deck 2 at 124,
  master handoff at 10 s) through the pure player at 8x with synthetic time
  into an unstarted `NetworkStack` + `ClockEngine`: locked at 128 with master 1
  before 10 s, exactly one handoff, then master 2 at 124; 2.5 s synthetic;
  no parse errors; wall time is the capture clock.
  R-T1b the same through Core and the HTTP routes at 8x in real time.
- R-T2 pause freezes position and feeding; resume continues without a time
  jump in the engine; seek to 12 s gives master 2 within 2 beats; clamps;
  speed snapping.
- R-T3 loop wrap resets (session cleared) and keeps running; end without loop
  holds state; Restart. R-T3b per-step budget, warm-up window, idle step cost.
- R-T4 isolation: replay stack never sends (`counters.sent` 0), claim
  `replay`, live session restored after stop, no replay rows in
  `<data>/tracklists/`. R-T4b with a live stack on loopback: ports
  50000..50002 are released during replay and the live stack is back within
  1 s of stop (skipped if the ports are busy).
- R-T5 outputs: OSC to a discard port sends nothing during a default replay;
  with `outputs:true` (speed 1) it sends; outputs with speed != 1 is 400.
- R-T6 names; R-T6b pcapng check messages; R-T6c fuzz (truncation at every
  byte of a capture, 2000 seeded mutations; accepted inputs are loaded and
  replayed); R-T6d real captures in `tests/captures` (validate, load, replay
  to the end at 8x with zero parse errors, device counts, S06 golden; skipped
  when absent); R-T6e route validation and uploads.
- R-T7 API shapes and errors. R-T8 3-thread hammer of start, stop, seek,
  pause, set, files and status (TSan). R-T9 `HttpServer` per-route limits over
  a real socket. R-T10 disabled by default and inert; disabling stops a
  replay. R-T11 `shuntsim --record` of the 20-minute `longset` scenario
  replays at 8x in 150 s synthetic with both handoffs (1, 2, 1).

## Acceptance and effort

A 1-hour capture replays at 8x in about 8 minutes with correct tracklist
(R-T11 checks the 20-minute case: 150 s at 8x); stopping restores the live
stack within 1 s (R-T4b, measured 5 ms on loopback). Effort: M (4 days).

## Deviations

1. Seam wording updated to FS-00 section 7: hooks only implement `step`; the
   host supplies `feed(..., wallMs)`, `resetEngine(clearSession)` and
   `setOutputsEnabled`; sessions are parked by `enterReplay`, not swapped by
   the feature (the draft's `ReplayHooks::feed/reset/swapSession`).
2. Engine time uses capture spacing, not `base + (ts - first) / speed` as
   drafted: dividing by speed made beat intervals disagree with the reported
   tempo, so the engine could not lock at 8x (R-T1 requires it).
3. Outputs can only be sent at speed 1 (start and `set` refuse otherwise);
   resume with outputs on re-seeks to realign the engine with the clock.
4. Seek also clears the replay session (backward seeks would otherwise create
   rows with time going backwards) and only feeds the 30 s before the target,
   so rows before that window are not rebuilt.
5. Captures folder: `FeatureHost` has no data-folder accessor, so the folder is
   the setting `capturesDir`, defaulting to `$HOME/.local/share/shunt/captures`
   (the `shunt_headless` default data folder). With `--data DIR` the user
   must set `capturesDir` to `DIR/captures` until a `FeatureHost::dataDir()`
   seam exists (core change, reported).
6. Added routes `GET status`, `GET info`, `POST set`; `files` returns an
   object with `dir` and `maxUploadBytes`; upload is implemented (was
   phase 2) with structure validation and returns 201.
7. Only little-endian pcapng is accepted (what Shunt, Wireshark and tcpdump
   write on x86 and ARM); `net::PcapngReader` assumes it too.
8. R-T5 uses OSC as the observable output (no MIDI port in CI); MIDI goes
   through the same `Core::timeline()` gate.
9. The FS-TEST counting-allocator check is not done: `step` allocates nothing
   itself, but `FeatureHost::feed` runs the Core event path, and a global
   `operator new` replacement in a shared test binary would clash with other
   test files. R-T3b checks the idle step cost instead.
10. Found while fuzzing: `net::PcapngReader::next()` computes
    `20 + capLen > bodyLen` in 32 bits, so a captured length near 2^32 wraps
    and the reader copies up to 64 KiB past its buffer (ASan heap overflow
    with a crafted file). Replay never reaches it because `checkPcapng`
    rejects such blocks first; the one-line fix belongs to `net/src/Pcapng.cpp`.
