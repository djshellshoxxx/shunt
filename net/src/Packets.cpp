// Written from publicly documented packet analysis; no code from EPL or GPL projects.
// Parsers and builders per ES-02. All parsers are allocation-free and never throw.
#include "shunt/net/Packets.h"
#include <cstring>
#include <algorithm>

namespace shunt::net {

const uint8_t kMagic[kMagicLen] = {0x51, 0x73, 0x70, 0x74, 0x31, 0x57, 0x6d, 0x4a, 0x4f, 0x4c};

namespace {

inline uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
inline uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
inline void put32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

void copyName(char* dst, const uint8_t* src) {
    std::memcpy(dst, src, 20);
    dst[20] = '\0';
}

void putName(uint8_t* dst, const char* name) {
    std::memset(dst, 0, 20);
    size_t n = name ? std::strlen(name) : 0;
    if (n > 20) n = 20;
    std::memcpy(dst, name, n);
}

// Common header for 50000: type at 0x0a, 0x00 at 0x0b, name at 0x0c, 0x01 at 0x20,
// kind at 0x21, length at 0x22.
void announceHeader(uint8_t* out, uint8_t type, const char* name, DeviceKind kind, uint16_t length) {
    std::memset(out, 0, length);
    std::memcpy(out, kMagic, kMagicLen);
    out[0x0a] = type;
    out[0x0b] = 0x00;
    putName(out + 0x0c, name);
    out[0x20] = 0x01;
    out[0x21] = uint8_t(kind);
    put16(out + 0x22, length);
}

// Common header for 50001/50002: type at 0x0a, name at 0x0b, 0x01 at 0x1f,
// subtype at 0x20, device number at 0x21, remaining length at 0x22.
void beatStatusHeader(uint8_t* out, uint8_t type, const char* name, uint8_t subtype, uint8_t number, size_t length) {
    std::memset(out, 0, length);
    std::memcpy(out, kMagic, kMagicLen);
    out[0x0a] = type;
    putName(out + 0x0b, name);
    out[0x1f] = 0x01;
    out[0x20] = subtype;
    out[0x21] = number;
    put16(out + 0x22, uint16_t(length - 0x24));
}

inline bool inRange(uint8_t v, std::initializer_list<uint8_t> allowed) {
    for (auto a : allowed) if (a == v) return true;
    return false;
}

} // namespace

bool hasMagic(const uint8_t* data, size_t len) {
    return data && len >= kMinPacketLen && std::memcmp(data, kMagic, kMagicLen) == 0;
}

double decodePitch(uint32_t raw) { return (raw & 0x00ffffffu) / 1048576.0; }
uint32_t encodePitch(double multiplier) {
    double v = multiplier * 1048576.0 + 0.5;
    if (v < 0) v = 0;
    if (v > 0x00ffffff) v = 0x00ffffff;
    return uint32_t(v);
}

// ---------------------------------------------------------------- 50000

std::optional<Event> parseAnnounce(const uint8_t* d, size_t len, int64_t recvNs, uint32_t srcIp) {
    if (!hasMagic(d, len)) return std::nullopt;
    Event ev;
    ev.recvTimeNs = recvNs;
    ev.srcIp = srcIp;
    const uint8_t type = d[0x0a];
    switch (type) {
    case 0x06: {
        if (len < kKeepAliveLen) return std::nullopt;
        ev.type = EventType::KeepAlive;
        auto& k = ev.keepAlive;
        copyName(k.name, d + 0x0c);
        const uint8_t kind = d[0x21];
        k.kind = kind >= 1 && kind <= 3 ? DeviceKind(kind) : DeviceKind::Unknown;
        k.number = d[0x24];
        std::memcpy(k.mac.data(), d + 0x26, 6);
        k.ip = be32(d + 0x2c);
        k.peerCount = d[0x30];
        k.cdj3000Compatible = d[0x35] == 0x64;
        ev.device = k.number;
        return ev;
    }
    case 0x0a: case 0x00: case 0x02: case 0x04: case 0x08: case 0x01: case 0x03: case 0x05: {
        ev.type = EventType::Claim;
        auto& c = ev.claim;
        c = ClaimEvent{};
        c.packetType = type;
        copyName(c.name, d + 0x0c);
        switch (type) {
        case 0x0a:
            if (len < kHelloLen) return std::nullopt;
            break;
        case 0x00:
            if (len < kClaim1Len) return std::nullopt;
            c.counter = d[0x24];
            std::memcpy(c.mac.data(), d + 0x26, 6);
            break;
        case 0x02:
            if (len < kClaim2Len) return std::nullopt;
            c.ip = be32(d + 0x24);
            std::memcpy(c.mac.data(), d + 0x28, 6);
            c.number = d[0x2e];
            c.counter = d[0x2f];
            c.autoFlag = d[0x31];
            break;
        case 0x04:
            if (len < 0x26) return std::nullopt;
            c.number = d[0x24];
            c.counter = d[0x25];
            break;
        case 0x08:
            if (len < 0x25) return std::nullopt;
            c.number = d[0x24];
            break;
        default: // mixer assignment 0x01, 0x03, 0x05: number at 0x24 when present
            if (len > 0x24) c.number = d[0x24];
            break;
        }
        ev.device = c.number;
        return ev;
    }
    default:
        return std::nullopt;
    }
}

// ---------------------------------------------------------------- 50001

std::optional<Event> parseBeatPort(const uint8_t* d, size_t len, int64_t recvNs, uint32_t srcIp) {
    if (!hasMagic(d, len)) return std::nullopt;
    Event ev;
    ev.recvTimeNs = recvNs;
    ev.srcIp = srcIp;
    ev.device = d[0x21];
    const uint8_t type = d[0x0a];
    switch (type) {
    case 0x28: {
        if (len < kBeatLen) return std::nullopt;
        ev.type = EventType::Beat;
        auto& b = ev.beat;
        b = BeatEvent{};
        b.nextBeatMs = be32(d + 0x24);
        b.secondBeatMs = be32(d + 0x28);
        b.nextBarMs = be32(d + 0x2c);
        b.fourthBeatMs = be32(d + 0x30);
        b.secondBarMs = be32(d + 0x34);
        b.eighthBeatMs = be32(d + 0x38);
        b.pitch = decodePitch(be32(d + 0x54));
        b.bpm100 = be16(d + 0x5a);
        b.beatInBar = d[0x5c];
        if (ev.device == kMixerDeviceNumber || b.beatInBar < 1 || b.beatInBar > 4) b.beatInBar = 0;
        b.effectiveBpm = b.bpm100 == 0xffff ? 0.0 : b.bpm100 / 100.0 * b.pitch;
        b.isMaster = false;
        return ev;
    }
    case 0x0b: {
        if (len < kPrecisePositionMinLen) return std::nullopt;
        ev.type = EventType::PrecisePosition;
        auto& p = ev.precise;
        p.trackLengthS = be32(d + 0x24);
        p.playheadMs = int32_t(be32(d + 0x28));
        p.pitchPercent = int32_t(be32(d + 0x2c)) / 64.0;
        p.bpm10 = be32(d + 0x38);
        return ev;
    }
    case 0x03: {
        if (len < 0x28) return std::nullopt;
        ev.type = EventType::OnAir;
        auto& o = ev.onAir;
        std::memset(o.channels, 0, sizeof o.channels);
        for (int i = 0; i < 4; ++i) o.channels[i] = d[0x24 + i] ? 1 : 0;
        o.channelCount = 4;
        if (len >= 0x35 && d[0x20] == 0x03) {
            o.channels[4] = d[0x2d] ? 1 : 0;
            o.channels[5] = d[0x2e] ? 1 : 0;
            o.channelCount = 6;
        }
        return ev;
    }
    case 0x02: case 0x26: case 0x27: case 0x2a: {
        ev.type = EventType::Control;
        ev.control.packetType = type;
        std::memset(ev.control.payload, 0, sizeof ev.control.payload);
        const size_t n = std::min<size_t>(8, len - 0x24);
        std::memcpy(ev.control.payload, d + 0x24, n);
        return ev;
    }
    default:
        return std::nullopt;
    }
}

// ---------------------------------------------------------------- 50002

std::optional<Event> parseStatusPort(const uint8_t* d, size_t len, int64_t recvNs, uint32_t srcIp) {
    if (!hasMagic(d, len)) return std::nullopt;
    Event ev;
    ev.recvTimeNs = recvNs;
    ev.srcIp = srcIp;
    ev.device = d[0x21];
    const uint8_t type = d[0x0a];
    if (type == 0x0a) {
        if (len < kMinCdjStatusLen) return std::nullopt;
        ev.type = EventType::PlayerStatus;
        auto& s = ev.status;
        s = PlayerStatusEvent{};
        s.packetLength = uint16_t(std::min<size_t>(len, 0xffff));
        s.activity = d[0x27];
        s.sourcePlayer = d[0x28];
        s.slot = d[0x29];
        s.trackType = d[0x2a];
        s.rekordboxId = be32(d + 0x2c);
        s.trackNumber = be16(d + 0x32);
        s.playState = d[0x7b];
        std::memcpy(s.firmware, d + 0x7c, 4);
        s.firmware[4] = '\0';
        s.hasFByte = len > 0xd0;
        s.flags = d[0x89];
        s.pitch1 = decodePitch(be32(d + 0x8c));
        s.bpm100 = be16(d + 0x92);
        s.p3 = d[0x9d];
        s.masterMeaning = d[0x9e];
        s.handoffTo = d[0x9f];
        s.beatNumber = be32(d + 0xa0);
        s.cueCountdown = be16(d + 0xa4);
        s.beatInBar = d[0xa6];
        if (s.beatInBar > 4) s.beatInBar = 0;
        s.nx = len > 0xcc ? d[0xcc] : 0;
        if (s.hasFByte) {
            s.playing = (s.flags & kFlagPlay) != 0;
            s.master = (s.flags & kFlagMaster) != 0;
            s.synced = (s.flags & kFlagSync) != 0;
            s.onAir = (s.flags & kFlagOnAir) != 0;
            s.bpmSync = (s.flags & kFlagBpmSync) != 0;
        } else {
            s.playing = inRange(s.playState, {3, 4, 7, 8, 9});
            s.master = s.masterMeaning != 0;
            s.flags = uint8_t((s.playing ? kFlagPlay : 0) | (s.master ? kFlagMaster : 0));
        }
        s.hasKey = len >= 0x15f;
        if (s.hasKey) std::memcpy(s.key, d + 0x15c, 3);
        return ev;
    }
    if (type == 0x29) {
        if (len < kMixerStatusLen) return std::nullopt;
        ev.type = EventType::MixerStatus;
        auto& m = ev.mixer;
        m.fromRekordbox = d[0x20] == 0x01;
        m.flags = d[0x27];
        m.master = (m.flags & kFlagMaster) != 0;
        m.bpm100 = be16(d + 0x2e);
        m.handoffTo = d[0x36];
        m.beatInBar = d[0x37];
        return ev;
    }
    return std::nullopt;
}

std::optional<Event> parseForPort(uint16_t port, const uint8_t* d, size_t len, int64_t recvNs, uint32_t srcIp) {
    switch (port) {
    case kAnnouncePort: return parseAnnounce(d, len, recvNs, srcIp);
    case kBeatPort: return parseBeatPort(d, len, recvNs, srcIp);
    case kStatusPort: return parseStatusPort(d, len, recvNs, srcIp);
    default: return std::nullopt;
    }
}

// ---------------------------------------------------------------- builders

size_t buildKeepAlive(uint8_t* out, const KeepAliveParams& p) {
    announceHeader(out, 0x06, p.name, p.kind, uint16_t(kKeepAliveLen));
    const uint8_t kindByte = p.kind == DeviceKind::Mixer ? 0x02 : 0x01;
    out[0x24] = p.number;
    out[0x25] = kindByte;
    std::memcpy(out + 0x26, p.mac.data(), 6);
    put32(out + 0x2c, p.ip);
    out[0x30] = p.peerCount;
    out[0x34] = kindByte;
    out[0x35] = p.cdj3000Compatible ? 0x64 : 0x00;
    return kKeepAliveLen;
}

size_t buildHello(uint8_t* out, const char* name, bool cdj3000Compatible, DeviceKind kind) {
    const size_t len = cdj3000Compatible ? kHelloLen3000 : kHelloLen;
    announceHeader(out, 0x0a, name, kind, uint16_t(len));
    out[0x24] = kind == DeviceKind::Mixer ? 0x02 : 0x01;
    if (cdj3000Compatible) out[0x25] = 0x64;   // TODO verify against a CDJ-3000 capture
    return len;
}

size_t buildClaimStage1(uint8_t* out, const char* name, uint8_t counter, const std::array<uint8_t, 6>& mac) {
    announceHeader(out, 0x00, name, DeviceKind::CDJ, uint16_t(kClaim1Len));
    out[0x24] = counter;
    out[0x25] = 0x01;
    std::memcpy(out + 0x26, mac.data(), 6);
    return kClaim1Len;
}

size_t buildClaimStage2(uint8_t* out, const char* name, uint32_t ip, const std::array<uint8_t, 6>& mac,
                        uint8_t number, uint8_t counter, uint8_t autoFlag) {
    announceHeader(out, 0x02, name, DeviceKind::CDJ, uint16_t(kClaim2Len));
    put32(out + 0x24, ip);
    std::memcpy(out + 0x28, mac.data(), 6);
    out[0x2e] = number;
    out[0x2f] = counter;
    out[0x30] = 0x01;
    out[0x31] = autoFlag;
    return kClaim2Len;
}

size_t buildClaimStage3(uint8_t* out, const char* name, uint8_t number, uint8_t counter) {
    announceHeader(out, 0x04, name, DeviceKind::CDJ, uint16_t(kClaim3Len));
    out[0x24] = number;
    out[0x25] = counter;
    return kClaim3Len;
}

size_t buildDefend(uint8_t* out, const char* name, uint8_t number) {
    announceHeader(out, 0x08, name, DeviceKind::CDJ, uint16_t(kDefendLen));
    out[0x24] = number;
    return kDefendLen;
}

size_t buildBeat(uint8_t* out, const BeatParams& p) {
    beatStatusHeader(out, 0x28, p.name, 0x00, p.number, kBeatLen);
    put32(out + 0x24, p.nextBeatMs);
    put32(out + 0x28, p.secondBeatMs);
    put32(out + 0x2c, p.nextBarMs);
    put32(out + 0x30, p.fourthBeatMs);
    put32(out + 0x34, p.secondBarMs);
    put32(out + 0x38, p.eighthBeatMs);
    put32(out + 0x54, encodePitch(p.pitch));
    put16(out + 0x5a, p.bpm100);
    out[0x5c] = p.beatInBar;
    out[0x5f] = p.number;
    return kBeatLen;
}

size_t buildCdjStatus(uint8_t* out, const CdjStatusParams& p) {
    const size_t len = std::max<size_t>(p.length, kMinCdjStatusLen + 1);
    beatStatusHeader(out, 0x0a, p.name, 0x03, p.number, len);
    out[0x27] = p.activity;
    out[0x28] = p.sourcePlayer;
    out[0x29] = p.slot;
    out[0x2a] = p.trackType;
    put32(out + 0x2c, p.rekordboxId);
    put16(out + 0x32, p.trackNumber);
    out[0x7b] = p.playState;
    std::memcpy(out + 0x7c, p.firmware, std::min<size_t>(4, std::strlen(p.firmware)));
    put32(out + 0x84, p.syncN);
    out[0x89] = p.flags;
    put32(out + 0x8c, encodePitch(p.pitch1));
    put16(out + 0x92, p.bpm100);
    out[0x9d] = p.p3;
    out[0x9e] = p.masterMeaning;
    out[0x9f] = p.handoffTo;
    put32(out + 0xa0, p.beatNumber);
    put16(out + 0xa4, p.cueCountdown);
    out[0xa6] = p.beatInBar;
    put32(out + 0xc0, encodePitch(p.pitch3));
    out[0xcc] = p.nx;
    if (len >= 0x15f) std::memcpy(out + 0x15c, p.key, 3);
    return len;
}

size_t buildMixerStatus(uint8_t* out, const MixerStatusParams& p) {
    beatStatusHeader(out, 0x29, p.name, 0x00, p.number, kMixerStatusLen);
    out[0x27] = p.flags;
    put16(out + 0x2e, p.bpm100);
    out[0x36] = p.handoffTo;
    out[0x37] = p.beatInBar;
    return kMixerStatusLen;
}

size_t buildOnAir(uint8_t* out, const uint8_t channels[6], bool sixChannels) {
    const size_t len = sixChannels ? 0x35 : 0x2d;
    beatStatusHeader(out, 0x03, "DJM-900NXS2", sixChannels ? 0x03 : 0x00, kMixerDeviceNumber, len);
    for (int i = 0; i < 4; ++i) out[0x24 + i] = channels[i] ? 1 : 0;
    if (sixChannels) {
        out[0x2d] = channels[4] ? 1 : 0;
        out[0x2e] = channels[5] ? 1 : 0;
    }
    return len;
}

size_t buildPrecisePosition(uint8_t* out, const char* name, uint8_t number, uint32_t trackLengthS,
                            int32_t playheadMs, double pitchPercent, uint32_t bpm10) {
    beatStatusHeader(out, 0x0b, name, 0x00, number, kPrecisePositionMinLen);
    put32(out + 0x24, trackLengthS);
    put32(out + 0x28, uint32_t(playheadMs));
    put32(out + 0x2c, uint32_t(int32_t(pitchPercent * 64.0)));
    put32(out + 0x38, bpm10);
    return kPrecisePositionMinLen;
}

} // namespace shunt::net
