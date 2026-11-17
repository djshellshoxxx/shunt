// FixtureBuilder (FS-TEST section 3): deterministic in-memory player-network captures built with the
// net/ Packets builders, optionally written as pcapng with PcapngWriter. Tests describe decks and
// timed changes; the same CaptureParams (including seed) always yield byte-identical captures.
// Time resolution is 1 ms: keep-alives every 1.5 s, status every 200 ms (+50 ms), beats at tempo.
// Original Shunt code.
#pragma once
#include "shunt/net/Packets.h"
#include "shunt/net/Pcapng.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace fixture {

struct DeckSpec {
    uint8_t number = 1;
    const char* name = "CDJ-3000";
    double bpm = 128.0;
    double pitch = 1.0;
    bool playing = true;
    bool master = false;
    bool onAir = true;            // F-byte on-air bit (and mixer channel when mixerOnAir)
    uint32_t trackId = 0;         // 0 = 1000 + number
    bool hasKey = false;          // CDJ-3000 only (status length 0x200)
    uint8_t key[3] = {0, 0, 0};
    uint8_t handoffTo = 0xff;
    uint32_t beatNumber = 1;      // next beat number; advances with each beat
    int beatInBar = 1;
};

struct Change {
    double atS;
    uint8_t deck;                              // device number (1..4)
    std::function<void(DeckSpec&)> apply;
};

struct CaptureParams {
    uint32_t seed = 1;
    double durationS = 10;
    std::vector<DeckSpec> decks;               // empty: a single master CDJ-3000 numbered 1
    bool mixer = true;                         // announce a DJM and send its status
    bool mixerOnAir = true;                    // send 0x03 packets from decks[i].onAir (channel = number)
    double jitterMs = 0;                       // gaussian delay on beats
    double lossPct = 0;                        // beat loss
    std::vector<Change> changes;
    int64_t epochNs = 1'700'000'000LL * 1'000'000'000LL;
};

inline std::vector<shunt::net::CapturedDatagram> buildCapture(CaptureParams p) {
    using namespace shunt::net;
    std::vector<CapturedDatagram> out;
    if (p.decks.empty()) { DeckSpec d; d.master = true; p.decks.push_back(d); }
    std::stable_sort(p.changes.begin(), p.changes.end(), [](const Change& a, const Change& b) { return a.atS < b.atS; });
    std::mt19937 rng(p.seed);
    std::normal_distribution<double> gauss(0.0, 1.0);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    constexpr uint32_t mixerIp = 0xc0a80121;
    const std::array<uint8_t, 6> mixerMac{0, 0xe0, 0x36, 0xbb, 0, 0x21};
    uint8_t buf[1024];

    struct Pend { int64_t atMs; uint64_t seq; CapturedDatagram d; };
    std::vector<Pend> delayed;
    uint64_t seq = 0;
    auto put = [&](int64_t ms, uint32_t srcIp, uint16_t port, size_t len) {
        CapturedDatagram d;
        d.timestampNs = p.epochNs + ms * 1'000'000LL;
        d.srcIp = srcIp; d.dstIp = 0xc0a801ff; d.srcPort = port; d.dstPort = port;
        d.payload.assign(buf, buf + len);
        return d;
    };
    auto periodMs = [](const DeckSpec& d) { return 60000.0 / (d.bpm * d.pitch); };
    std::vector<double> nextBeat(p.decks.size());
    for (size_t i = 0; i < p.decks.size(); ++i) nextBeat[i] = 200.0 + uni(rng) * periodMs(p.decks[i]);

    size_t nextChange = 0;
    const int64_t endMs = int64_t(p.durationS * 1000);
    for (int64_t ms = 0; ms < endMs; ++ms) {
        while (nextChange < p.changes.size() && p.changes[nextChange].atS * 1000 <= double(ms)) {
            for (auto& d : p.decks) if (d.number == p.changes[nextChange].deck) p.changes[nextChange].apply(d);
            ++nextChange;
        }
        for (size_t i = 0; i < delayed.size();) {   // flush due delayed beats in order
            if (delayed[i].atMs <= ms) {
                delayed[i].d.timestampNs = p.epochNs + ms * 1'000'000LL;
                out.push_back(std::move(delayed[i].d));
                delayed.erase(delayed.begin() + long(i));
            } else ++i;
        }
        if (ms % 1500 == 0) {
            for (auto& d : p.decks) {
                KeepAliveParams k; k.name = d.name; k.number = d.number; k.mac = {0, 0xe0, 0x36, 0xaa, 0, d.number};
                k.ip = 0xc0a80100u + d.number; k.peerCount = uint8_t(p.decks.size() + 1);
                out.push_back(put(ms, k.ip, kAnnouncePort, buildKeepAlive(buf, k)));
            }
            if (p.mixer) {
                KeepAliveParams m; m.name = "DJM-900NXS2"; m.kind = DeviceKind::Mixer; m.number = kMixerDeviceNumber;
                m.mac = mixerMac; m.ip = mixerIp;
                out.push_back(put(ms, mixerIp, kAnnouncePort, buildKeepAlive(buf, m)));
            }
        }
        if (ms % 200 == 50) {
            const DeckSpec* master = nullptr;
            for (auto& d : p.decks) {
                if (d.master) master = &d;
                CdjStatusParams s; s.name = d.name; s.number = d.number;
                s.length = std::strcmp(d.name, "CDJ-3000") == 0 ? 0x200 : 0x11c;
                s.slot = 3; s.trackType = 1; s.rekordboxId = d.trackId ? d.trackId : 1000u + d.number; s.trackNumber = d.number;
                s.playState = d.playing ? 3 : 5;
                s.flags = uint8_t((d.playing ? shunt::net::kFlagPlay : 0) | (d.master ? shunt::net::kFlagMaster : 0) |
                                  (d.onAir ? shunt::net::kFlagOnAir : 0));
                s.pitch1 = d.pitch; s.bpm100 = uint16_t(std::lround(d.bpm * 100)); s.masterMeaning = d.master ? 2 : 0;
                s.handoffTo = d.handoffTo; s.beatNumber = d.beatNumber; s.beatInBar = uint8_t(d.beatInBar);
                s.nx = s.length == 0x200 ? 0x1f : 0x0f;
                if (d.hasKey && s.length >= 0x15f) std::memcpy(s.key, d.key, 3);
                out.push_back(put(ms, 0xc0a80100u + d.number, kStatusPort, buildCdjStatus(buf, s)));
            }
            if (p.mixer) {
                MixerStatusParams m; m.flags = 0xd0;
                m.bpm100 = master ? uint16_t(std::lround(master->bpm * master->pitch * 100)) : 0xffff;
                out.push_back(put(ms, mixerIp, kStatusPort, buildMixerStatus(buf, m)));
                if (p.mixerOnAir) {
                    uint8_t ch[6] = {0, 0, 0, 0, 0, 0};
                    for (auto& d : p.decks) if (d.number >= 1 && d.number <= 4) ch[d.number - 1] = d.onAir && d.playing;
                    out.push_back(put(ms, mixerIp, kBeatPort, buildOnAir(buf, ch, false)));
                }
            }
        }
        for (size_t i = 0; i < p.decks.size(); ++i) {
            DeckSpec& d = p.decks[i];
            if (!d.playing || double(ms) < nextBeat[i]) continue;
            BeatParams b; b.name = d.name; b.number = d.number; b.pitch = d.pitch; b.bpm100 = uint16_t(std::lround(d.bpm * 100));
            b.beatInBar = uint8_t(d.beatInBar);
            const uint32_t per = uint32_t(periodMs(d));
            b.nextBeatMs = per; b.secondBeatMs = 2 * per; b.nextBarMs = uint32_t((5 - d.beatInBar) * per);
            b.fourthBeatMs = 4 * per; b.secondBarMs = b.nextBarMs + 4 * per; b.eighthBeatMs = 8 * per;
            const size_t len = buildBeat(buf, b);
            const double delay = gauss(rng) * p.jitterMs;
            const bool lost = p.lossPct > 0 && uni(rng) * 100 < p.lossPct;
            if (!lost) {
                CapturedDatagram dg = put(ms, 0xc0a80100u + d.number, kBeatPort, len);
                if (delay > 0) delayed.push_back({ms + int64_t(delay), seq++, std::move(dg)});
                else out.push_back(std::move(dg));
            }
            nextBeat[i] += periodMs(d);
            d.beatInBar = d.beatInBar % 4 + 1;
            ++d.beatNumber;
        }
        if (p.mixer && ms % 500 == 200) {
            const DeckSpec* master = nullptr;
            for (auto& d : p.decks) if (d.master) master = &d;
            if (master) {
                BeatParams b; b.name = "DJM-900NXS2"; b.number = kMixerDeviceNumber;
                b.bpm100 = uint16_t(std::lround(master->bpm * master->pitch * 100)); b.pitch = 1.0;
                out.push_back(put(ms, mixerIp, kBeatPort, buildBeat(buf, b)));
            }
        }
    }
    return out;
}

inline bool writeCapture(const std::vector<shunt::net::CapturedDatagram>& dgrams, const std::string& path) {
    shunt::net::PcapngWriter w;
    if (!w.open(path)) return false;
    for (auto& d : dgrams) if (!w.write(d)) return false;
    w.close();
    return true;
}

inline bool writeCapture(const CaptureParams& p, const std::string& path) { return writeCapture(buildCapture(p), path); }

} // namespace fixture
