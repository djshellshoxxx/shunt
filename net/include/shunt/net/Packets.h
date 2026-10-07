// Written from publicly documented packet analysis; no code from EPL or GPL projects.
// Parsers and builders for the player-network packets, per ES-02.
#pragma once
#include "shunt/net/Events.h"
#include <cstdint>
#include <cstddef>
#include <optional>

namespace shunt::net {

constexpr size_t kMagicLen = 10;
extern const uint8_t kMagic[kMagicLen];
constexpr size_t kMinPacketLen = 0x24;

constexpr uint16_t kAnnouncePort = 50000;
constexpr uint16_t kBeatPort = 50001;
constexpr uint16_t kStatusPort = 50002;

constexpr size_t kKeepAliveLen = 54;      // 0x36
constexpr size_t kHelloLen = 0x25;
constexpr size_t kHelloLen3000 = 0x26;
constexpr size_t kClaim1Len = 0x2c;
constexpr size_t kClaim2Len = 0x32;
constexpr size_t kClaim3Len = 0x2a;
constexpr size_t kDefendLen = 0x29;
constexpr size_t kBeatLen = 0x60;
constexpr size_t kMixerStatusLen = 0x38;
constexpr size_t kMinCdjStatusLen = 0xcc;
constexpr size_t kPrecisePositionMinLen = 0x3c;
constexpr uint8_t kMixerDeviceNumber = 0x21;

bool hasMagic(const uint8_t* data, size_t len);
double decodePitch(uint32_t raw);            // raw / 1048576.0
uint32_t encodePitch(double multiplier);

// Parsers. recvNs is the receive timestamp; srcIp the datagram source (host order).
std::optional<Event> parseAnnounce(const uint8_t* data, size_t len, int64_t recvNs, uint32_t srcIp = 0);
std::optional<Event> parseBeatPort(const uint8_t* data, size_t len, int64_t recvNs, uint32_t srcIp = 0);
std::optional<Event> parseStatusPort(const uint8_t* data, size_t len, int64_t recvNs, uint32_t srcIp = 0);
std::optional<Event> parseForPort(uint16_t port, const uint8_t* data, size_t len, int64_t recvNs, uint32_t srcIp = 0);

// Builders. Each writes into `out` (must hold at least the documented length)
// and returns the number of bytes written.
struct KeepAliveParams {
    const char* name = "Shunt";
    DeviceKind kind = DeviceKind::CDJ;
    uint8_t number = 0;
    std::array<uint8_t, 6> mac{};
    uint32_t ip = 0;
    uint8_t peerCount = 1;
    bool cdj3000Compatible = true;
};
size_t buildKeepAlive(uint8_t* out, const KeepAliveParams& p);
size_t buildHello(uint8_t* out, const char* name, bool cdj3000Compatible, DeviceKind kind = DeviceKind::CDJ);
size_t buildClaimStage1(uint8_t* out, const char* name, uint8_t counter, const std::array<uint8_t, 6>& mac);
size_t buildClaimStage2(uint8_t* out, const char* name, uint32_t ip, const std::array<uint8_t, 6>& mac,
                        uint8_t number, uint8_t counter, uint8_t autoFlag);
size_t buildClaimStage3(uint8_t* out, const char* name, uint8_t number, uint8_t counter);
size_t buildDefend(uint8_t* out, const char* name, uint8_t number);

// Builders used by the simulator and (v2) Lead mode.
struct BeatParams {
    const char* name = "CDJ-2000NXS2";
    uint8_t number = 1;
    uint32_t nextBeatMs = 0, secondBeatMs = 0, nextBarMs = 0, fourthBeatMs = 0, secondBarMs = 0, eighthBeatMs = 0;
    double pitch = 1.0;
    uint16_t bpm100 = 0xffff;
    uint8_t beatInBar = 1;
};
size_t buildBeat(uint8_t* out, const BeatParams& p);

struct CdjStatusParams {
    const char* name = "CDJ-2000NXS2";
    uint8_t number = 1;
    size_t length = 0x11c;
    uint8_t activity = 0, sourcePlayer = 0, slot = 3, trackType = 1;
    uint32_t rekordboxId = 0;
    uint16_t trackNumber = 0;
    uint8_t playState = 0;
    const char* firmware = "1.00";
    uint32_t syncN = 0;
    uint8_t flags = 0;
    double pitch1 = 1.0;
    uint16_t bpm100 = 0xffff;
    uint8_t p3 = 1;
    uint8_t masterMeaning = 0;
    uint8_t handoffTo = 0xff;
    uint32_t beatNumber = 0xffffffff;
    uint16_t cueCountdown = 0x01ff;
    uint8_t beatInBar = 1;
    double pitch3 = 1.0;
    uint8_t nx = 0x0f;
    uint8_t key[3] = {0, 0, 0};
};
size_t buildCdjStatus(uint8_t* out, const CdjStatusParams& p);

struct MixerStatusParams {
    const char* name = "DJM-900NXS2";
    uint8_t number = kMixerDeviceNumber;
    uint8_t flags = 0xd0;
    uint16_t bpm100 = 0xffff;
    uint8_t handoffTo = 0xff;
    uint8_t beatInBar = 1;
};
size_t buildMixerStatus(uint8_t* out, const MixerStatusParams& p);
size_t buildOnAir(uint8_t* out, const uint8_t channels[6], bool sixChannels);
size_t buildPrecisePosition(uint8_t* out, const char* name, uint8_t number, uint32_t trackLengthS,
                            int32_t playheadMs, double pitchPercent, uint32_t bpm10);

// Flag bits in the status F byte.
constexpr uint8_t kFlagPlay = 0x40, kFlagMaster = 0x20, kFlagSync = 0x10, kFlagOnAir = 0x08, kFlagBpmSync = 0x02;

} // namespace shunt::net
