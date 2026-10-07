# ES-01: Network stack (detailed engineering spec)

Status: draft 0.1, normative. Research: `docs/research/01-pro-dj-link-protocol.md`,
`docs/research/03-timing-appliance-integrations.md` section 3.

The network stack joins an AlphaTheta / Pioneer DJ player network, keeps a
live table of devices, tracks the tempo master, and delivers timestamped
beat and status events to the clock engine (ES-03). It is a plain C++17
library (`shunt_net`) with a thin socket abstraction so that it is testable
with injected packets and a replayer, without hardware.

---

## 1. Goals

- N1. Discover every device on the segment within 2 s of start and detect
  departures within 10 s.
- N2. Claim a device number without ever disturbing real players: never
  take 1 to 4 unless the user selects Lead mode; back off on any defend.
- N3. Timestamp every beat packet at receive time with a monotonic clock
  with at most 50 µs added jitter on Linux (`SO_TIMESTAMPNS`) and at most
  200 µs elsewhere.
- N4. Identify the tempo master and handoffs within one status period
  (200 ms).
- N5. Survive rekordbox, a DJM, an XDJ-XZ, an XDJ-AZ, an Opus Quad and
  CDJ-3000s on the same segment without crashing or kicking anything off.
- N6. Never send anything to a player except keep-alives, claim packets,
  defend packets and (Lead mode only) status and beats. No NFS, no dbserver
  unless the user enables metadata (ES-05).

## 2. Modes

| Mode | Device number | Sends | Receives | Use |
|---|---|---|---|---|
| Passive | none (no keep-alive) | nothing | beats and on-air (broadcast only; no unicast status reaches us) | coexist with rekordbox on the same host; tempo and phase only, no master detection from status (uses mixer beats and beat packets only) |
| Follow (default) | self-assigned 7 to 15 | keep-alive, claim, defend | everything including unicast status | normal operation |
| Lead | user-chosen 1 to 6 | plus status at 5 Hz and beats | everything | metadata via dbserver; becoming master (v2) |

Passive mode is required because two Pro DJ Link stacks on one host
conflict. In Passive mode the stack binds 50001 with port reuse only and
does not bind 50000 or 50002.

## 3. Sockets and interfaces

- Interface selection: enumerate interfaces; prefer the one on which the
  first keep-alive is seen on 50000 (listen on all for 3 s at start); user
  override by name. Store the chosen interface's address, netmask and
  broadcast address.
- Sockets: three UDP sockets bound to 0.0.0.0 on 50000, 50001, 50002 with
  `SO_REUSEADDR` (and `SO_REUSEPORT` where available) and `SO_BROADCAST`.
  Do not bind to the interface address (broadcasts are not received that
  way on Linux and macOS). Filter inbound datagrams by source subnet.
- Receive buffers 256 KB. `SO_TIMESTAMPNS` on Linux; otherwise
  `steady_clock::now()` immediately after `recvfrom`.
- Link-local: if no DHCP lease within 10 s on the chosen interface, assign
  169.254.x.y with x.y from the last two MAC bytes (CDJ convention), after
  RFC 3927 ARP probing. Handled by the OS network manager on the appliance
  (RS-02); the desktop app only warns.
- One instance per host guard: a named mutex or lock file; a second
  instance starts in Passive mode with a warning.

## 4. Device table

```cpp
struct Device {
    uint8_t number; DeviceKind kind;       // CDJ, Mixer, Rekordbox, Unknown
    std::string name; std::array<uint8_t,6> mac; uint32_t ip;
    uint8_t peerCount; bool cdj3000Compatible; // keep-alive byte 0x35 == 0x64
    Model model;                            // derived from name and status length
    steady_clock::time_point lastKeepAlive, lastStatus, lastBeat;
};
```

Updated from keep-alives (0x06) on 50000. Expiry after 10 s without a
keep-alive. Model derived from the name string ("CDJ-3000", "XDJ-AZ",
"DJM-900NXS2", "rekordbox", "OPUS-QUAD") with a fallback to status length
(ES-02 section 4). Capability flags per model (from research 02 section 4):
sendsBeats, sendsPrecisePosition, supportsNumbers5and6, dbserverOk,
opusQuadQuirks.

## 5. Device-number claim state machine

```
Idle ──start──> Watching(4 s, collect keep-alives)
Watching ──> Claiming(n = first free in [7..15] for Follow, user n for Lead)
Claiming: hello x3 (300 ms) → stage1 x3 (N=1..3) → stage2 x3 (auto=0x01, our IP+MAC) → stage3 x3 (n)
   any 0x08 defend naming n ──> Claiming(next n)   (Lead: fail with "number in use")
   mixer 0x05 ──> Active early
Claiming done ──> Active: keep-alive every 1500 ms; on 0x02/0x04 claim from
   another device for our n ──> send 0x08 defend
Active ──network down──> Idle (re-watch on recovery)
```

Keep-alive bytes exactly as ES-02 section 2 with byte 0x35 = 0x64 and the
CDJ-3000-compatible hello. Device name "Shunt" (20-byte field). Kind 0x01
(CDJ). In Passive mode this machine never runs.

## 6. Master tracking

```
NoMaster ──status with F.master (or Mm != 0)──> Master(n)
Master(n) ──status(n).Mh = m──> Handoff(n → m)      (pre-arm m's filter)
Handoff(n → m) ──status(m).F.master──> Master(m)    (emit MasterChanged)
Master(n) ──n stops sending status for 2 s──> NoMaster
NoMaster, mixer present ──> MixerMaster (use 0x29 BPM and mixer beats; bar phase held)
```

Beat packets are tagged `isMaster` when their sender equals the current
master. In Passive mode (no unicast status) the master is inferred as: the
device whose beat packets have `isTempoMaster`-consistent BPM equal to the
mixer's beat BPM, else the most recently playing player; the UI shows
"inferred".

## 7. Events delivered to the clock engine

All events carry `recvTime` (monotonic ns) and `device`.

| Event | From | Fields |
|---|---|---|
| `BeatEvent` | 0x28 | bpm100, pitch, effectiveBpm, beatInBar, nextBeatMs, nextBarMs, isMaster |
| `PrecisePositionEvent` | 0x0b | playheadMs, pitchPercent, bpm10 (CDJ-3000 only; off by default) |
| `PlayerStatusEvent` | 0x0a | playState (P1), flags F, pitch1, bpm100 or unknown, beatNumber or unknown, beatInBar, master Mm, handoffTo Mh, rekordboxId, slot, trackType, trackNumber, onAir, firmware |
| `MixerStatusEvent` | 0x29 | flags F, bpm100, handoffTo, beatInBar |
| `OnAirEvent` | 0x03 | channels[6] |
| `MasterChanged` | master tracker | from, to |
| `DeviceEvent` | device table | joined, left, model |

Delivered through an SPSC queue (capacity 4096) to the clock engine thread
and fanned out to UI, logger and OSC from there. Parsing happens on the
receive thread; it must be allocation-free (fixed-size events).

## 8. Threads

| Thread | Priority | Does |
|---|---|---|
| Announce | normal | 50000 receive, device table, claim machine, keep-alive timer |
| Beat | highest (SCHED_FIFO 80 on Linux where permitted) | 50001 receive, timestamp, parse, enqueue |
| Status | high | 50002 receive, parse, master tracker, enqueue |
| Clock engine | high (SCHED_FIFO 70) | ES-03 |

No locks between receive threads and the clock engine; the device table is
a seqlock-published snapshot.

## 9. Diagnostics and capture

- Packet trace: optional ring buffer of raw datagrams with timestamps,
  dumpable to pcapng (libpcap not required; write the pcapng blocks
  directly). "Record session" in the UI writes continuously to the data
  partition.
- Replay: `--replay file.pcapng --speed 1.0 --jitter-ms 0 --loss 0` feeds
  the same parsers from a file with the original timing; the whole stack
  above the socket layer runs unchanged. Replay is the primary CI input.
- Counters: packets per type per device, parse errors, dropped events,
  defend packets seen, timestamp source in use.

## 10. Test requirements

- N-T1. Parser fuzz: random bytes and truncated real packets never crash
  and never produce an event with an out-of-range field.
- N-T2. Replay of the dysentery `powerup.pcapng` and `to-virtual.pcapng`
  produces the expected device table and master sequence (golden JSON).
- N-T3. Claim machine against ShuntSim (RS-04): with numbers 1 to 4 taken,
  Follow mode ends on 7; with 7 defended, ends on 8; Lead mode on a taken
  number reports failure without sending further claims.
- N-T4. CDJ-3000 keep-alive bytes: golden byte comparison including 0x35 =
  0x64.
- N-T5. Master handoff in ShuntSim produces exactly one `MasterChanged` with
  the right from and to, within 200 ms of the new master asserting.
- N-T6. Passive mode never sends a datagram (socket mock asserts).
