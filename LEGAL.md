# Legal

## Compatibility statement (ES-04 section 6)

Shunt works with PRO DJ LINK compatible players and mixers from AlphaTheta /
Pioneer DJ. PRO DJ LINK, rekordbox and Pioneer DJ are trademarks of their
respective owners. Shunt is an independent product and is not affiliated
with, endorsed, licensed or certified by AlphaTheta Corporation or Pioneer
Corporation.

This wording appears in the README, the desktop app's About box, and the
footer of the web pages served by the desktop app and the Shunt Box. Product
copy calls the input "the player network"; "PRO DJ LINK compatible" is used
only in the statement above. No AlphaTheta or Pioneer DJ logos are used, and
the words "Bridge" and "certified" are never used in naming (RS-06).

## Clean-room rule (RS-06)

The protocol code (`net/`, `meta/`) is written from the documented layouts in
`docs/specs/ES-02-packet-formats.md` and `docs/research/01-pro-dj-link-protocol.md`.
No byte arrays, comments or identifiers are copied from EPL or GPL sources
(beat-link, dysentery, Crate Digger, Carabiner, Beat Link Trigger). Each
protocol source file carries a header stating this.

## Security posture

Read-only by default. In Follow mode Shunt sends only keep-alive, claim and
defend packets on the player network; in Passive mode it sends nothing. In
Lead (metadata) mode it additionally opens dbserver TCP connections to the
players to read track titles. Shunt never writes to a player, never decrypts
Device Library Plus, and does not use NFS.

## Third-party components

| Component | Licence | Use |
|---|---|---|
| JUCE 8 (7.0.12 for MinGW cross builds) | JUCE licence (Starter / Indie / Pro) or AGPLv3 | desktop app, headless app |
| Ableton Link | GPLv2+, or proprietary licence from Ableton | Link output; CMake option `SHUNT_WITH_LINK` |
| civetweb | MIT | local HTTP and WebSocket server |
| nlohmann/json | MIT | settings and state JSON |
| Monocypher | BSD-2-Clause or CC0 | Ed25519 licence-key signatures |

**Ableton Link:** builds made with `SHUNT_WITH_LINK=ON` (the default) include
GPLv2+ code, so they may be distributed only under the GPL until the
proprietary Link licence (requested jointly with Pacemaker, PROJECT-PLAN
phase 0) is granted. Commercial closed-source builds before that must be
configured with `-DSHUNT_WITH_LINK=OFF`; MIDI clock and OSC still ship.

**JUCE:** commercial distribution requires a JUCE licence matching revenue
(Starter is free under US$20k); the same account as Pacemaker is used.

## Product licence

Perpetual per-user key file valid on 3 machines; the Box image is licensed
per device (RS-06). Key files are Ed25519-signed and verified offline
(`core/include/shunt/core/Licence.h`, `tools/shunt_keygen`). Release builds
configured with `SHUNT_REQUIRE_LICENCE=ON` run as Shunt Monitor (no outputs)
without a valid key.
