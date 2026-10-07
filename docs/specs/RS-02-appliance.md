# RS-02: Shunt Box appliance (regular spec)

Status: draft 0.1. Research: `docs/research/03-timing-appliance-integrations.md` section 2.

## Hardware reference design

| Part | Choice | Notes |
|---|---|---|
| Computer | Raspberry Pi 4 2 GB (CM4 Lite on a carrier for a cased product) | Pi 5 supported, not required |
| Player network | onboard gigabit Ethernet | link-local fallback from the MAC, as the players do |
| Config and Link network | Wi-Fi: hotspot `Shunt-xxxx` by default, client mode selectable | Link multicast bound to this interface by default, selectable |
| MIDI DIN out | PL011 UART on GPIO14/15 with `midi-uart0` and `disable-bt`, 6N138 input not needed (out only), 33 and 10 ohm series resistors; or Pisound when an audio click is wanted | mini-UART unusable; Pi 5 needs the RP1 overlay |
| USB MIDI | class-compliant interface on USB-A, and USB gadget mode on USB-C so a laptop sees "Shunt MIDI" | gadget mode means the laptop powers the Pi |
| Status | SSD1306 OLED over I2C (BPM, master, bar:beat, peers, IP); one WS2812 pixel for beat and bar; one button (short: bar offset, long: reset Wi-Fi) | |
| Storage | 16 GB A1 microSD | read-only root with overlayfs, rw `/data` partition |

BOM about $80 to $110 without Pisound, about $190 with. Retail target $279.

## Software

- `shunt_headless`: JUCE console app (juce_core, events, audio_basics,
  audio_devices, osc), Link SDK, civetweb (MIT) for HTTP and WebSocket.
  `MessageManager::runDispatchLoopUntil` in the main loop with a SIGTERM
  handler.
- systemd service: `Restart=always`, `WatchdogSec=10` (write `WATCHDOG=1`
  to `$NOTIFY_SOCKET` directly), `Nice=-10`, `LimitRTPRIO=95`,
  `CAP_SYS_NICE`. Hardware watchdog on.
- Networking by NetworkManager profiles: eth0 `ipv4.method=auto`,
  `ipv4.link-local=fallback`, `ipv4.dhcp-timeout=10`; wlan0 hotspot
  profile; no forwarding between interfaces.
- avahi on wlan0 only: `shunt.local`.
- Web UI on port 80: live status (WebSocket 10 Hz), outputs config, bar
  offset, latency, profile, record and replay, tracklist download, update
  (flips the overlay, applies a signed bundle, reboots).
- `/data`: `config.json`, tracklist NDJSON with fsync per row, session
  captures, logs with rotation.
- Updates: signed tarball over the web UI or from a USB stick.

## Out of scope for v1 of the Box

Pisound audio click as default; PoE; custom enclosure beyond an
off-the-shelf case; battery.
