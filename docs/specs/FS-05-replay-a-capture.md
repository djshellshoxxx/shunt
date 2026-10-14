# FS-05: Replay a capture

Status: draft 0.1. Depends on FS-00. Feature id `replay`. UI `64-replay.js`.

## Goal

Run a recorded pcapng through the whole stack (engine, outputs, tracklist,
UI) for demos, soundchecks without a booth, bug reports and regression tests.
`shunt_cli --replay` already does a console-only version.

## Behaviour

- **Files:** list pcapng files in `<data>/captures/`. For each: name, size,
  modified time, and (lazily, cached) datagram count, duration, devices seen.
- **Start:** pick a file, speed (0.25, 0.5, 1, 2, 4, 8), loop on/off,
  "send outputs" (default OFF: replay must not drive a real lighting rig or
  hardware sequencer unless the user opts in).
- **Isolation:** while replaying, the live network stack is stopped (no
  sockets, no claim, no keep-alives sent) and the UI shows a persistent amber
  banner "REPLAY: name  mm:ss / mm:ss  x1". Recording is disabled. The real
  tracklist session is parked and a separate in-memory session is used;
  replay rows are never written to `<data>/tracklists/` and are discarded on
  stop (export during replay is allowed from the replay session).
- **Controls:** pause, resume, seek (to second `t`: restart the engine and
  fast-forward by feeding datagrams up to `t` with synthetic time, then
  continue), stop (restore live stack and parked session).
- **Loop and end:** at end of file with loop: reset engine, device table and
  session, continue; without loop: hold last state and show "ended" with
  Restart and Stop.
- **Engine reset points:** start, seek, loop wrap (new `ClockEngine`
  instance and `DeviceTable` cleared) so the timeline never sees a time jump.
- **Time mapping:** replay time `t` maps to monotonic `base + t / speed`;
  datagram `recvNs = base + (cap.ts - first.ts) / speed`; wall clock for the
  tracklist is `captureStartWall + t` where `captureStartWall` is the first
  packet's pcapng timestamp.

## API (`/api/f/replay/`)

| Route | Body | Notes |
|---|---|---|
| `GET files` | | list as above |
| `POST start` | `{file, speed, loop, outputs}` | file is a bare name; path separators rejected |
| `POST pause`, `resume`, `stop` | | |
| `POST seek` | `{t}` | seconds, clamped |
| `POST upload?name=x.pcapng` | raw bytes | optional (phase 2); needs `HttpServer::setMaxBody(256 MB)` for this route; name sanitised to `[A-Za-z0-9._-]`, must end `.pcapng`, no overwrite without `?replace=1` |

Status: `{active, file, positionS, durationS, speed, paused, loop, ended, outputs}`.

## Design

- `ReplaySource` (pure, `app/src/features/Replay.cpp`): holds the parsed
  datagrams (`PcapngReader::readAll`, cap 500 MB file / 5 million datagrams),
  `advanceTo(tReplayNs, sink)` feeds due datagrams; no threads.
- Core needs one new seam, added in FS-00 so this feature touches no core
  file: `FeatureHost::enterReplay(ReplayHooks&)` / `leaveReplay()` where
  `ReplayHooks` supplies `feed(port, data, len, recvNs, srcIp)`, `reset()`,
  `swapSession(unique_ptr<TracklistSession>)`, `setOutputsEnabled(bool)`. The
  net loop, while a replay is active, calls `feature.onTick` and skips
  `stack.poll`. (FS-00 must include these hooks; see its acceptance list.)
- Parsing happens on the HTTP thread (can take seconds for big files) before
  the swap; the swap itself is a quick pointer move under the Core lock.

## UI

Page `#/replay`: file table (name, size, duration, devices), Start with the
three options, transport bar (play/pause, scrub slider, speed select, loop),
upload button (phase 2). The global banner is a widget in the shell slot
`banner`.

## Tests

- R-T1 fixture builder writes a 20 s pcapng from the test packet builders
  (steady 128 BPM, one master handoff at 10 s). Replay at 8x: engine locks,
  one handoff, bpm 128 and then the new master's bpm.
- R-T2 pause freezes position; resume continues; seek to 12 s gives the new
  master within 2 beats.
- R-T3 loop wrap resets and keeps running; end without loop holds.
- R-T4 isolation: no datagram is sent by the stack during replay (mock
  sender counts 0); live session untouched after stop; replay rows not
  persisted.
- R-T5 outputs off by default: `OutputManager` receives no events (mock);
  with outputs on, MIDI generator runs.
- R-T6 path traversal (`../x`, absolute path, NUL) rejected; non-pcapng and
  truncated files give clean errors.
- R-T7 API shapes. R-T8 TSan: start/stop/seek hammered from 3 threads.

## Acceptance and effort

A 1-hour capture replays at 8x in about 8 minutes with correct tracklist;
stopping restores the live stack within 1 s (loopback test). Effort: M (4 days).
