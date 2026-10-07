# Research 02: Market, competitors, hardware compatibility and legal

Status: research notes, 7 October 2026. Inputs to `docs/specs/PS-01-product.md`
and `docs/DIFFERENTIATION.md`. The research proxy blocked direct reads of
pioneerdj.com, alphatheta.com, deepsymmetry.org guide pages, djtechtools,
cdm.link, Reddit, Gearspace, Elektronauts, the Ableton and Pioneer forums,
tc-supply.com, nowplayingapp.com and resolume.com; facts from those come from
search snippets ("via search") and should be verified before publishing.

---

## 1. Existing solutions and their limits

### 1.1 Pioneer / AlphaTheta "PRO DJ LINK Bridge" (official, free)

- A free Mac and Windows app that "allows you to synchronize real time
  information present on a PRO DJ LINK network to external lighting, video,
  SFX applications and devices via a protocol called TCNet". It registers as
  a TCNet SERVER node and relays to TCNet CLIENT devices.
- **Outputs: TCNet only.** No Ableton Link, no MIDI clock, no OSC, no SMPTE.
  Anything else needs a TCNet client (ShowKontrol, Resolume Arena, MagicQ,
  GrandVJ, Pangolin Beyond).
- Supported hardware (current page, via search): CDJ-3000X, CDJ-3000,
  CDJ-1500X, DJM-V10/V10-LF, DJM-A9; older manual lists CDJ-2000NXS2,
  DJM-900NXS2, TOUR1. **Not** Opus Quad, XDJ-AZ, XDJ-RX3, XDJ-XZ,
  XDJ-1000MK2.
- Network: a 100 Mbps switch on the Pro DJ Link side and "an additional
  Ethernet switching hub" for the TCNet side, so a two-NIC laptop in the
  booth.
- rekordbox not required, but tracks must be rekordbox-analysed for grids
  and phrases.
- Certified clients: Resolume Arena 7.12+ ("Pioneer DJ Sync", Arena only),
  ChamSys MagicQ (certified October 2023), ArKaos GrandVJ 2.6.5+, TC Supply
  ShowKontrol. Pioneer's 2019 "network information licensed to VFX
  companies" announcement is the formal programme.
- Limits for Shunt's audience: TCNet is a lighting protocol musicians cannot
  consume; no Link or MIDI; two-NIC laptop; flagship-only support.

### 1.2 TC Supply ShowKontrol and TCNet

- TC Supply co-developed the licensed data path with Pioneer in 2017. TCNet
  spec V3.5.1B is published as "open protocol, free to use" (CueSync, via
  search).
- Pricing (via search): ShowKontrol **Live $1,999** (or $99/mo), **Club
  $249** (or $25/mo), PRO lease only. ShowKontrol does output Ableton Link
  ("Ableton Link is not part of TCNet, but can be obtained working using
  ShowKontrol", Resolume forum). Playlist history export is PRO tier.
- Designed and priced for lighting and visual crews.

### 1.3 ProDJLink.com (commercial timecode software)

- Mac and Windows. Outputs Ableton Link (tempo from the master deck),
  timecode, MIDI clock, lighting triggers. **Editor €898, Executor €1,989**,
  free trial. Show-control pricing.

### 1.4 CueSync (cuesync.live)

- "Auto-syncs your stage lighting and visuals to Pioneer DJ CDJs over PRO DJ
  LINK: beat phase, BPM and track metadata feed cue triggers processed in
  under 5 ms." Integrations: grandMA2/3, Eos, MagicQ, Avolites, Resolume,
  disguise, TouchDesigner, UE5, QLab, Pangolin, OSC, MSC; TCNet V3.5.1B.
  Price not found.

### 1.5 rekordbox's built-in Ableton Link

- Only in **Performance mode** (rekordbox as the playback engine). In Export
  or Link-Export mode (the normal CDJ workflow) rekordbox does not publish
  Link from CDJ tempo, and it needs a laptop with its own audio interface.
  Irrelevant to a DJ playing USB on club CDJs.

### 1.6 DJM mixer MIDI clock

- **DJM-900NXS2** (Pioneer forum, via search): "doesn't send MIDI clock, only
  BPM data"; the BPM sent depends on source (rekordbox BPM for Link tracks,
  measured for audio). Mod Wiggler: Ableton "showing the tempo at about 5.5
  BPM slower than where the mixer is at". DJTechTools 2015: "anytime the
  tempo is changed the MIDI sync/clock will typically fall out of place and
  the only way to get them back in sync is to restart the MIDI clock".
- **DJM-V10** (CDM): "reliable, stable MIDI clock output ... Beat FX
  quantization and MIDI clock all sync to the BPM of the active channel".
  Caveat: a connected player must be playing and internal quantize toggled.
  Channel/FX-selected BPM, no bar phase.
- **DJM-A9**: "can send MIDI clock messages; does not support the input of
  MIDI" (Pioneer FAQ). **DJM-750MK2**: clock on USB-B, no Start/Stop.
  **DJM-S11**: MIDI send-only. **Euphonia**: no evidence of LAN Pro DJ Link
  or clock; treat as no.
- Ableton external sync complaints (Ableton forum, via search): "when
  sending 120 BPM externally, Ableton registers 94.8 or 132.5 sometimes and
  takes roughly 2 bars until it is synced"; "MIDI clock jitter is related to
  ASIO buffer size". This is the core reason to publish Link, not only MIDI
  clock.

### 1.7 Beat Link Trigger and Carabiner (Deep Symmetry, free, EPL-2.0)

- Clojure and Java. BLT 8.0.0 (July 2025) requires Java 21. Registers a
  virtual CDJ.
- Link modes: Triggers (legacy), Passive (Link follows the players, never
  controls them), Full (bidirectional; requires "Use Real Player Number"
  1 to 4). Bar alignment default; a Latency field (default 1 ms).
- Known caveat: "CDJ-3000 precise position packets currently exhibit too
  much jitter for use when synchronizing with audio sources over Ableton
  Link"; workaround is to disable them. "Pioneer gear only syncs to
  individual beats from Link, not downbeats."
- Unreleased changelog: a new `beat-carabiner` "enables timeline nudging for
  cleaner-sounding synchronization", so phase smoothing is still being
  worked on in 2025 to 2026.
- Headless: Open Beat Control (OSC API, Java 9+, "haven't yet tested
  Raspberry Pi") embeds Carabiner (GPLv2 daemon, port 17000).
- Setup difficulty: Java install, Ethernet into the DJ switch, device number
  choice, optional Carabiner daemon. Powerful but intimidating; the UI is an
  expressions and REPL environment.

### 1.8 Lightweight bridges

- **djlink2midi** (Node): master tempo to MIDI clock; "only sends midi
  clock data, not time code, so you won't get exact beat quantization, just
  the tempo"; "Node.js isn't ideal for hard realtime ... minor clock drift
  and jitter". Dormant.
- **python-prodj-link**: monitoring GUI plus `midiclock.py`. Dormant.
- **CDJ_Clock** (macOS, C): MIDI clock, "works between 60-180bpm". Old.
- **canihazmidi**: a concept for a Pi plus pisound Pro DJ Link to MIDI box.
  Never built. Validates the appliance idea.
- **GaspardMenou/djlink** (2025): XDJ-AZ monitor with MIDI clock, a MIDI
  note per beat for grandMA2, OSC `/djlink/bpm`, `/playing`, `/deck` at
  5 Hz and `/djlink/beat`. Proves XDJ-AZ sends beats, status and waveforms
  over Ethernet.
- **PROcrush/pro-dj-link-monitor** (MIT, 2025): Mac and Windows monitor plus
  WebSocket, OSC, TCNet capture, LTC; "Not supported: MIDI clock or Ableton
  Link outputs"; CDJ-3000X and 1500X "untested".
- **fiverecords/SuperTimecodeConverter** (C++17, JUCE, MIT, all three OSes):
  the closest technical cousin. Pro DJ Link input at 30 Hz, SMPTE/MTC/LTC
  and Art-Net out, Ableton Link BPM publishing, 24 ppqn clock from CDJ or
  Denon BPM, OSC BPM forwarding, StageLinQ input. Timecode-centric; Link
  output is BPM only, no beat or bar phase claim.
- **prolink-connect / prolink-tools** (TypeScript, Electron, MIT): OBS
  overlays and device status, "still in the beta phase". No Link or MIDI.
- **Now Playing** (nowplayingapp.com): Mac and Windows, free tier plus
  **Pro $24.99 one-time**; CDJ-3000/2000 and XDJ-XZ via Pro DJ Link; "does
  not work with the XDJ-RX3 in standalone mode because it doesn't support
  ProDJ-Link". Saves playlist history; Twitch, Discord, OBS integrations.
- **Stagehand** (official AlphaTheta iPad app, €23.99): remote settings and
  monitoring for DJM-A9 and CDJ-3000; exposes "beatgrid, BPM, and time
  information to help create effective lighting displays". AlphaTheta sees
  lighting as a paying audience.
- **Mixxx**: no Pro DJ Link support found. **djay Pro** added CDJ-3000X Pro
  DJ Link (November 2025) and CDJ-1500X (June 2026) as a player, not a
  bridge.
- **Denon StageLinQ**: no official API; community libs chrisle/StageLinq
  (TypeScript: discovery, StateMap, BeatInfo, TimeSync), dzelionis
  denon-stageLinQ-BeatInfo (Python). Resolume and Now Playing support it
  natively. A StageLinQ input is a credible v2 for Shunt.

## 2. User needs and pain points

1. **"Sync Ableton to CDJs" is a decade-old ask.** Pioneer forum "Sync CDJs
   with Ableton Live", "Ableton Link" (2016+), Ableton forum "Push 3, Ableton
   link and CDJs" (2024), Elektronauts "Can I control Elektron machines via
   CDJ?": "CDJs do not have a MIDI DIN interface and don't send MIDI-clock
   over USB ... The simplest solution is to have the DJ beat-matching the
   CDJs to your Elektrons."
2. **DJM clock is tempo-only and from BPM analysis.** No bar phase, no
   Start/Stop on the 750MK2; users restart clocks after tempo changes.
3. **Ableton external MIDI sync is bad.** Misreads, two bars to lock,
   buffer-size jitter. Everyone who can uses Link.
4. **Bar and downbeat misalignment.** Pioneer gear "only syncs to individual
   beats from Link, not downbeats"; Opus Quad "beat/bar alignment with
   Ableton Link is impossible (only tempo syncing works)".
5. **CDJ-3000 precise-position jitter** forces disabling that feature in
   BLT.
6. **Needing a laptop (and Java) in the booth.** rekordbox Link needs
   Performance mode plus a second interface; Bridge needs two NICs; BLT
   needs Java 21. The canihazmidi concept and Open Beat Control show
   appetite for an appliance.
7. **Master handoff**: Link-based DJ apps show "decks can jump to the
   previous tempo rather than the new one" and "temporary glitch in audio
   playback" when phase re-aligns (Algoriddim community). A bridge must slew
   on master change.
8. **Wi-Fi, port and device-number clashes**: "Never run Beat Link on the
   same machine as rekordbox, they compete for the same network ports";
   virtual CDJ needs numbers 1 to 4 for metadata "only if you are using
   fewer than 4 CDJs"; XDJ-RX/RX2 "crash when BLT starts"; Opus Quad
   USB-Ethernet adapter "unreliable".
9. **Unsupported hardware**: Opus Quad and XDJ-RX3 are the most common
   prosumer units and either do not speak the protocol (RX3 standalone) or
   speak a crippled version (Opus Quad: no beat packets, encrypted library).
10. **Lighting and VJ people** want beat and phrase without a $2k licence:
    Onyx forum "Pro DJ Link" request, TouchDesigner forum "Clock: using the
    CDJ2000 to sync videos", Resolume forum threads; grandMA users bridge
    Link to MIDI via ShowCockpit.
11. **Security scare (2025)**: AlphaTheta's "Important Notice: Security
    Vulnerability in PRO DJ LINK" (NFS export with hardcoded auth; affects
    CDJ-3000X/3000/2000NXS2/1500X/900NXS/XDJ-1000MK2/700/AZ/XZ; fixed in
    rekordbox 7.2.17 and 6.8.7). Venues may be warier of unknown devices on
    the booth LAN. Shunt must be visibly read-only and never touch NFS by
    default.

## 3. Adjacent value

- **Tracklist and reporting**: PRS for Music pays DJ royalties from set
  lists and uses DJ Monitor at venues since 2018; Music Business Worldwide:
  "Setlist reporting isn't an optional extra". Tools: ryser.id, set79,
  Trakd, TrackList, mixgraph. **All are audio-fingerprint based**; none logs
  from the network, so unreleased edits and exact on-air times are lost.
  ShowKontrol PRO (lease) and Now Playing are the only network loggers. A
  Pro DJ Link log has exact title, artist, label, load time, on-air time,
  deck and source. Clear gap.
- **Visuals and lighting**: Resolume has native Link since v6 and Pro DJ
  Link sync (Arena, via Bridge or ShowKontrol) since 7.12; grandMA3 has no
  Link; MagicQ via TCNet; TouchDesigner via Link CHOP or OSC. Shunt's OSC
  plus Link covers Resolume and TouchDesigner. A TCNet server output would
  make it a Bridge replacement for MagicQ and Arena, but TCNet certification
  is controlled by AlphaTheta and TC Supply.
- **Streaming overlays**: prolink-tools and Now Playing own this; Shunt
  should expose WebSocket JSON state and a browser-source page rather than
  compete on overlay design.
- **Phrase and structure countdown**: rekordbox 6+ exports PSSI (song
  structure) in ANLZ `.EXT` files; CDJ-3000 sends phrase analysis to
  rekordbox, and Opus Quad sends "rekordbox lighting packets containing
  phrase analysis". BLT 7.x already does phrase triggers. Phrase data is on
  the network for CDJ-3000-class gear and on USB for NXS2 (but USB reads are
  what the 2025 advisory concerns).

## 4. Hardware compatibility matrix

| Model | Pro DJ Link to players | Beats (50001) | Status/on-air (50002) | Precise position | Phrase on net | Notes |
|---|---|---|---|---|---|---|
| CDJ-2000 (non-nexus) | yes | partial | yes, less info | no | no | "works fairly well (with less information)" |
| CDJ-2000NXS / 900NXS | yes | yes | yes | no | no | fully supported by beat-link |
| CDJ-2000NXS2 / TOUR1 | yes | yes | yes | no | no (USB ANLZ only) | reference platform |
| CDJ-3000 / 3000X | yes | yes | yes | yes (jittery) | yes | players 1 to 6; 3000X in the 2025 security list |
| CDJ-1500X (2025/26) | yes (Bridge lists it) | likely | yes | ? | ? | new; verify |
| XDJ-1000 / 1000MK2 | yes | yes | yes | no | no | "works fairly well" |
| XDJ-700 | yes | yes | yes | no | no | cheapest genuine peer |
| XDJ-XZ | yes (2 decks) | yes | yes | no | no | BLT 0.6.0+; Now Playing supported |
| XDJ-RX / RX2 / RX3 | **no** in standalone (LINK port is rekordbox only) | no | no | no | no | RX/RX2 "crash when BLT starts" |
| XDJ-AZ (2025) | yes | yes | yes | ? | ? | GaspardMenou/djlink shows beats, BPM, master; encrypted Device Library Plus |
| Opus Quad | **partial** | **no** | yes (beat number in status, ±200 ms) | special mode only | yes (lighting packets) | tempo OK, bar alignment impossible; encrypted library |
| DJM-900NXS2 / V10 / A9 / TOUR1 | yes (on-air, master, BPM) | mixer sends beats when master | yes (channels on air) | n/a | n/a | A9 and V10 also MIDI clock |
| DJM-750MK2 | LAN for rekordbox link | no | limited | no | no | MIDI clock on USB, no Start/Stop |
| DJM-S11, Euphonia | rekordbox/Serato only | no | no | no | no | unsupported |
| rekordbox (PC/Mac) as device | yes (Performance/Link-export) | yes | yes | no | no | port conflict: cannot co-host with a virtual CDJ on the same machine |

Special handling: device number 1 to 4 vs 5 to 6 (CDJ-3000 allows 6;
metadata lookups traditionally need a real number 1 to 4); master handoff
slewing; Opus Quad tempo-only mode with an explicit "no bar" indicator;
XDJ-AZ library encryption (passphrase known to the community but legally
sensitive; keep tracklist from status packets and dbserver where possible).

## 5. Market and pricing

- **Installed base**: Digital DJ Tips: "at least 70% of the global DJ
  equipment market"; other sources claim near-total share of professional
  club booths. Directional, but CDJ-3000 plus DJM-A9/V10 rider dominance is
  uncontested.
- **Comparable prices**: BLT, Open Beat Control, prolink-tools,
  SuperTimecodeConverter, pro-dj-link-monitor: free. Now Playing Pro
  $24.99. Stagehand €23.99. ShowKontrol Club $249, Live $1,999, $25 to $99
  per month. ProDJLink.com €898 / €1,989. Hardware: Circuit Happy Missing
  Link (Pi Zero W based, about €198, discontinued), Missing Link Junior
  $240, ML:2m eurorack $250; E-RM multiclock €449 / €519; Blokas pisound
  from $89.
- **Precedent for a Pi appliance**: Missing Link shipped a Pi Zero W as a
  Link-to-MIDI/CV box; canihazmidi proposed exactly "Pro DJ Link to MIDI on
  Pi plus pisound".
- **Suggested positioning**: app $49 to $79 one-time (between Now Playing
  $25 and ShowKontrol Club $249); free monitor-only tier; box $249 to $349
  (same band as Missing Link Jr, under E-RM); optional venue licence $199
  to $299 for installs with tracklist export. Lighting users will compare
  against the free Bridge plus their console's TCNet; sell them OSC and
  Link convenience and the tracklist log, not TCNet.

## 6. Legal and naming

- **Trademark**: "PRO DJ LINK" is a registered trademark of AlphaTheta
  Corporation (US registration 2021, Justia via search); "Pioneer DJ" is a
  Pioneer Corporation mark used under licence; "rekordbox" likewise.
- **How others word it**: prolink-tools: "not affiliated, associated,
  authorized, endorsed by, or in any way officially connected with
  AlphaTheta Corporation (Pioneer DJ)". SuperTimecodeConverter: "based on
  independent community research". pro-dj-link-monitor: "independent,
  unofficial interoperability project". Deep Symmetry: "unofficial,
  unsanctioned implementation ... Use at your own risk!"
- **Takedown history**: none found. Beat Link (since 2016), dysentery,
  prolink-connect, python-prodj-link, Now Playing and ProDJLink.com have
  operated for years with no reported cease and desist. AlphaTheta's
  responses have been a paid TCNet path (2019) and the 2025 security
  advisory, which targets NFS exposure, not beat and status listeners.
- **Risky areas**: decrypting Device Library Plus (Opus Quad, XDJ-AZ);
  reading USB contents over NFS after the advisory; implying certification.
  The Carabiner binary is GPLv2 and beat-link is EPL-2.0: a C++ app should
  use the Link SDK directly and implement the protocol from the public
  packet analysis rather than port EPL code.
- **Safe wording**: "Shunt works with PRO DJ LINK compatible players and
  mixers from AlphaTheta / Pioneer DJ. PRO DJ LINK, rekordbox and Pioneer
  DJ are trademarks of their respective owners. Shunt is an independent
  product and is not affiliated with, endorsed, licensed or certified by
  AlphaTheta Corporation or Pioneer Corporation." Avoid "Bridge",
  "certified", and any logo use.

## 7. What makes Shunt different

1. **Musician-first outputs**: Link with beat and bar phase (not just BPM),
   MIDI clock on DIN and USB with Start/Continue aligned to the downbeat,
   OSC, from a single native binary with no Java, no daemon, no two-NIC
   laptop.
2. **Appliance form**: a headless Pi box in the booth switch. The only
   shipped precedent (Missing Link) did not speak Pro DJ Link.
3. **Master-handoff and jitter engineering as the core feature**: slewed
   tempo on master change, a filter on beat packets, precise-position
   de-jitter, the thing BLT documents as unsolved.
4. **Network-derived tracklist and cue sheet** with load and on-air
   timestamps, deck and source; nobody under $100 does this from the
   network.
5. **Honest hardware tiers** in-app: full, tempo-only, unsupported, with
   reasons.

## 8. Additional feature candidates, ranked by value divided by effort

1. WebSocket JSON state plus OBS browser-source overlay (high, low).
2. Phrase countdown (bars to next chorus or drop) via PSSI from CDJ-3000
   and Opus lighting packets to OSC and display (high for live musicians,
   medium).
3. Mixer on-air and fader to OSC; mute MIDI clock when the deck is off-air
   (medium, low).
4. StageLinQ input for Denon Prime (high for Denon venues, medium).
5. TCNet server emulation for MagicQ and Arena (high for lighting,
   medium-high, trademark caution).
6. DIN-sync and CV clock out on the box (low effort with a HAT).
7. LTC/MTC timecode per deck (medium, niche).
8. Link to Pro DJ Link reverse sync (low value, high risk; BLT has it).
9. Opus Quad / XDJ-AZ library decryption (legal risk; defer).

## 9. Risks

- Protocol drift: new firmware or post-2025 hardening could add
  authentication; modular protocol layer and a quick update channel.
- Hardware expectations: prosumer buyers own Opus Quad and RX3; a clear
  pre-sale compatibility page is essential.
- Legal: trademark misuse or library decryption could invite the first
  cease and desist in this space; Link SDK licence for closed-source sale.
- Competition from free: BLT and Open Beat Control are improving;
  SuperTimecodeConverter is JUCE and MIT and could add bar-phase Link;
  AlphaTheta could add Link to Bridge or a DJM.
- Timing precision: Pi Ethernet plus USB MIDI jitter; a DIN HAT with a
  hardware UART is needed to meet E-RM-class expectations; Wi-Fi must be
  disallowed on the Pro DJ Link side.
- Venue IT: the 2025 advisory may make venues forbid unknown devices;
  promote read-only operation and no NFS access.

## Sources

- Pioneer PRO DJ LINK Bridge manual: https://www.pioneerdj.com/-/media/pioneerdj/downloads/pro-dj-link-bridge/pro-dj-link-bridge_manual_v12_en.pdf ; product page https://www.pioneerdj.com/en/product/software-interfaces/pro-dj-link-bridge/
- Pioneer DJ news 2019: https://www.pioneerdj.com/en-us/news/2019/pro-dj-link-network-information-licensed-to-vfx-companies/ ; MagicQ 2023: https://www.pioneerdj.com/ja/news/2023/pro-dj-link-bridge-now-officially-supports-chamsys-magicq/
- ArKaos GrandVJ: https://pro.arkaos.com/blog/2019/03/pioneer-pro-dj-link-bridge-integration-grandvj
- Resolume: https://resolume.com/support/en/sync-to-pioneer-dj-players ; https://www.resolume.com/forum/viewtopic.php?p=85145 ; CDM https://cdm.link/2022/06/resolume-7-12-now-syncs-your-live-visuals-with-pioneer-dj-gear-among-other-goodies/
- TC Supply: https://www.tc-supply.com/showkontrol-club ; https://www.tc-supply.com/showkontrol-live ; DJTechTools 2017 https://djtechtools.com/2017/10/12/cdjs-will-send-track-data-visualslighting-techs-showkontrol/
- ProDJLink.com: https://www.prodjlink.com/ ; https://www.prodjlink.com/help/ableton-link ; https://www.prodjlink.com/buy
- CueSync: https://www.cuesync.live/
- rekordbox Link thread: https://forums.pioneerdj.com/hc/en-us/community/posts/360061909132-Rekordbox-6-0-Ableton-Link-Feature-How-Is-It-Intended-To-Work
- DJM MIDI: https://forums.pioneerdj.com/hc/en-us/community/posts/214971263-DJM-900nxs2-midi-issues ; https://www.modwiggler.com/forum/viewtopic.php?t=241610 ; https://cdm.link/pioneer-v10-midi/ ; https://support.pioneerdj.com/hc/en-us/articles/15997858603545-Are-MIDI-connections-and-signal-transmission-available ; https://community.pioneerdj.com/hc/en-us/community/posts/22977714435993-Midi-clock
- DJTechTools: https://djtechtools.com/2017/07/19/decoding-pioneer-pro-link-connect-cdjs-ableton-link/ ; https://djtechtools.com/2018/10/08/pro-dj-link-5-secret-features-of-pioneer-djs-protocol/
- Ableton forum ext sync: https://forum.ableton.com/viewtopic.php?t=245900 ; https://forum.ableton.com/viewtopic.php?t=205893
- Elektronauts: https://www.elektronauts.com/t/can-i-control-elektron-machines-via-cdj/122405
- Deep Symmetry: https://github.com/Deep-Symmetry/beat-link-trigger ; https://raw.githubusercontent.com/Deep-Symmetry/beat-link-trigger/main/doc/modules/ROOT/pages/Link.adoc ; https://github.com/Deep-Symmetry/beat-link ; https://github.com/Deep-Symmetry/beat-carabiner ; https://github.com/Deep-Symmetry/open-beat-control ; https://github.com/Deep-Symmetry/dysentery ; https://djl-analysis.deepsymmetry.org/djl-analysis/packets.html
- Opus Quad analysis: https://github.com/kyleawayan/opus-quad-pro-dj-link-analysis ; XDJ-AZ monitor: https://github.com/GaspardMenou/djlink
- Bridges: https://github.com/Suva/pro-dj-link-to-midi ; https://github.com/flesniak/python-prodj-link ; https://github.com/g-zi/CDJ_Clock ; https://github.com/IljaN/canihazmidi ; https://github.com/PROcrush/pro-dj-link-monitor ; https://github.com/fiverecords/SuperTimecodeConverter ; https://github.com/evanpurkiser/prolink-tools
- Now Playing: https://www.nowplayingapp.com/product/now-playing-pro ; https://www.nowplayingapp.com/compatibility ; https://help.nowplayingapp.com/reference/integrations/prodjlink/
- Stagehand: https://www.pioneerdj.com/en/product/software-interfaces/stagehand/
- StageLinQ: https://github.com/chrisle/StageLinq ; https://github.com/dzelionis/denon-stageLinQ-BeatInfo ; https://community.enginedj.com/t/stagelinq-protocol-api-availability-part-2/38825
- Lighting and VJ: https://forum.obsidiancontrol.com/t/pro-dj-link/7945 ; https://forum.derivative.ca/t/clock-using-the-cdj2000-to-sync-videos-in-touch-designer/5690
- Link handoff issues: https://community.algoriddim.com/t/ableton-link-not-working-as-hoped/24565
- Tracklist and royalties: https://www.prsformusic.com/royalties/dj-royalties ; https://www.musicbusinessworldwide.com/setlist-reporting-isnt-an-optional-extra-its-a-fundamental-part-of-the-live-music-economy ; https://ryser.id/compare/best-dj-set-track-id-tools
- Market: https://www.digitaldjtips.com/pioneer-alphatheta-industry-standard/ ; https://alphatheta.com/en/information/alphathetas-next-generation-professional-dj-player-cdj-3000x/
- Hardware prices: https://www.signalsounds.com/circuit-happy-missing-link-junior-clock-generator-ableton-link-midi ; https://www.attackmagazine.com/reviews/gear-software/e-rm-multiclock/ ; https://blokas.io/pisound/
- Legal and security: https://alphatheta.com/en/company/trademarks/ ; https://trademarks.justia.com/792/40/pro-dj-79240803.html ; https://alphatheta.com/en/information/important-notice-security-vulnerability-in-pro-dj-link/ ; https://ra.co/news/85749
