// Shunt network stack: fixed-size event structs delivered to the clock engine
// (ES-01 section 7). Allocation-free: every event is a POD of bounded size.
#pragma once
#include <cstdint>
#include <cstddef>
#include <array>

namespace shunt::net {

enum class DeviceKind : uint8_t { Unknown = 0, CDJ = 1, Mixer = 2, Rekordbox = 3 };

enum class Model : uint8_t {
    Unknown = 0, CDJ3000, NXS2, Nexus, PreNexus, XDJ, XZ, AZ, Opus, Mixer, Rekordbox
};

struct Capabilities {
    bool sendsBeats = true;
    bool sendsPrecisePosition = false;
    bool supportsNumbers5and6 = false;
    bool dbserverOk = true;
    bool opusQuadQuirks = false;
    bool hasKey = false;
    bool hasLoops = false;
    bool embeddedMixer = false;
    bool hasFByte = true;       // false for pre-nexus
    bool verified = true;       // false => "unverified model" badge
};

enum class EventType : uint8_t {
    None = 0,
    KeepAlive,          // 0x06 on 50000
    Claim,              // 0x0a hello, 0x00/0x02/0x04 stages, 0x08 defend, 0x01/0x03/0x05 mixer assign
    Beat,               // 0x28 on 50001
    PrecisePosition,    // 0x0b on 50001
    OnAir,              // 0x03 on 50001
    Control,            // 0x02 fader start, 0x26/0x27 handoff, 0x2a sync control
    PlayerStatus,       // 0x0a on 50002
    MixerStatus,        // 0x29 on 50002 (also rekordbox subtype 0x01)
    MasterChanged,      // master tracker
    Device              // device table
};

struct KeepAliveEvent {
    char name[21];
    DeviceKind kind;
    uint8_t number;
    std::array<uint8_t, 6> mac;
    uint32_t ip;                 // host byte order
    uint8_t peerCount;
    bool cdj3000Compatible;      // byte 0x35 == 0x64
};

struct ClaimEvent {
    uint8_t packetType;          // 0x0a, 0x00, 0x02, 0x04, 0x08, 0x01, 0x03, 0x05
    char name[21];
    uint8_t number;              // claimed / defended number (0 if n/a)
    uint8_t counter;             // stage counter (0 if n/a)
    uint8_t autoFlag;            // stage 2: 0x01 auto, 0x02 specific
    std::array<uint8_t, 6> mac;
    uint32_t ip;
};

struct BeatEvent {
    uint16_t bpm100;             // 0xffff unknown
    double pitch;                // multiplier, 1.0 = neutral
    double effectiveBpm;
    uint8_t beatInBar;           // 1..4
    uint32_t nextBeatMs, secondBeatMs, nextBarMs, fourthBeatMs, secondBarMs, eighthBeatMs;
    bool isMaster;
};

struct PrecisePositionEvent {
    uint32_t trackLengthS;
    int32_t playheadMs;
    double pitchPercent;
    uint32_t bpm10;              // 0xffffffff unknown
};

struct PlayerStatusEvent {
    uint8_t playState;           // P1
    uint8_t flags;               // F byte (synthesised for pre-nexus)
    bool playing, master, synced, onAir, bpmSync;
    double pitch1;               // multiplier
    uint16_t bpm100;             // 0xffff unknown
    uint32_t beatNumber;         // 0xffffffff unknown
    uint8_t beatInBar;
    uint8_t masterMeaning;       // Mm
    uint8_t handoffTo;           // Mh, 0xff none
    uint32_t rekordboxId;
    uint8_t slot, trackType, sourcePlayer, activity, p3, nx;
    uint16_t trackNumber;
    uint16_t cueCountdown;
    char firmware[5];
    uint16_t packetLength;
    bool hasFByte;
    bool hasKey;
    uint8_t key[3];
};

struct MixerStatusEvent {
    uint8_t flags;
    bool master;
    uint16_t bpm100;
    uint8_t handoffTo;
    uint8_t beatInBar;
    bool fromRekordbox;
};

struct OnAirEvent {
    uint8_t channels[6];
    uint8_t channelCount;        // 4 or 6
};

struct ControlEvent {
    uint8_t packetType;          // 0x02, 0x26, 0x27, 0x2a
    uint8_t payload[8];
};

struct MasterChangedEvent {
    uint8_t from;                // 0 = none
    uint8_t to;                  // 0 = none, 0x21 = mixer
    bool inferred;
};

struct DeviceEvent {
    enum Kind : uint8_t { Joined, Left, ModelChanged } kind;
    Model model;
    DeviceKind deviceKind;
};

struct Event {
    EventType type = EventType::None;
    int64_t recvTimeNs = 0;      // monotonic
    uint8_t device = 0;          // device number of the sender
    uint32_t srcIp = 0;
    union {
        KeepAliveEvent keepAlive;
        ClaimEvent claim;
        BeatEvent beat;
        PrecisePositionEvent precise;
        PlayerStatusEvent status;
        MixerStatusEvent mixer;
        OnAirEvent onAir;
        ControlEvent control;
        MasterChangedEvent masterChanged;
        DeviceEvent deviceEvent;
    };
    Event() : keepAlive{} {}
};

} // namespace shunt::net
