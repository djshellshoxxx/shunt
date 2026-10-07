# RS-06: Licensing, legal and distribution (regular spec)

Status: draft 0.1. Research: `docs/research/02-market-competitors-hardware-legal.md` sections 5 and 6, `docs/research/01-pro-dj-link-protocol.md` section 9.

## Third-party code

| Component | Licence | Decision |
|---|---|---|
| JUCE 8 | Starter free under US$20k, Indie, Pro | same account as Pacemaker |
| Ableton Link | GPLv2+ or proprietary | one licence request covering Pacemaker and Shunt; feature flag |
| civetweb | MIT | use |
| Catch2 | BSL-1.0 | tests only |
| Kaitai rekordbox_pdb.ksy (from Crate Digger) | EPL-2.0 / MPL-2.0 secondary | use under MPL-2.0 only if NFS metadata ships; keep in its own module |
| beat-link, dysentery code | EPL | **not used**; protocol written from the published analysis |
| Carabiner | GPLv2 | not used |
| prolink-cpp, SuperTimecodeConverter | MIT | may borrow with attribution |
| node-tcnet (reference only) | MIT | reference for the P2 TCNet output |

## Clean-room rule

Protocol code is written from the documented layouts in
`docs/specs/ES-02-packet-formats.md`. No byte arrays, comments or
identifiers are copied from EPL or GPL sources. Each parser file carries a
header stating this.

## Trademarks and wording

Use the wording in ES-04 section 6 everywhere. Never "Bridge", never
"certified", never a logo. The input is "the player network" in product
copy; "PRO DJ LINK compatible" only in the compatibility statement.

## Security posture

Read-only by default: Shunt sends only keep-alives, claim and defend
packets in Follow mode and nothing in Passive mode. NFS metadata is off by
default and labelled. This is stated on the compatibility page and in the
venue documentation, with reference to AlphaTheta's 2025 advisory.

## Product licence and pricing

Perpetual per-user key file, 3 machines; the Box image is licensed per
device. Prices per ES-04 section 3. Channels: own site via Paddle or Lemon
Squeezy; Gumroad for the image; KVR and DJ forums for discovery; a
compatibility page as the main landing content.
