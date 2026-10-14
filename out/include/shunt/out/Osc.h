// OSC 1.0 encoder and output with controller profiles (ES-05 section 3).
#pragma once
#include "shunt/clock/ClockEngine.h"
#include "shunt/net/Events.h"
#include "shunt/net/Socket.h"
#include "shunt/out/OutputStatus.h"
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace shunt::out {

using OscArg = std::variant<int32_t, float, std::string>;

struct OscMessage {
    std::string address;
    std::vector<OscArg> args;
};

std::vector<uint8_t> encodeOsc(const OscMessage& m);
// Bundle with the immediate time tag.
std::vector<uint8_t> encodeOscBundle(const std::vector<OscMessage>& msgs);

class IOscTransport {
public:
    virtual ~IOscTransport() = default;
    virtual bool send(const uint8_t* data, size_t len) = 0;
};

// UDP transport over the net socket layer.
class UdpOscTransport : public IOscTransport {
public:
    UdpOscTransport(std::unique_ptr<net::IUdpSocket> sock, uint32_t ip, uint16_t port)
        : sock_(std::move(sock)), ip_(ip), port_(port) {}
    bool send(const uint8_t* d, size_t n) override { return sock_->send(d, n, ip_, port_); }
private:
    std::unique_ptr<net::IUdpSocket> sock_;
    uint32_t ip_;
    uint16_t port_;
};

enum class OscProfile : uint8_t { Generic, Resolume, GrandMA3, MagicQ, QLab };

struct OscConfig {
    OscProfile profile = OscProfile::Generic;
    bool shuntMessages = true;                 // also send /shunt/* with a non-generic profile
    int qlabMasterCue = 0;                     // 0 = disabled
    int qlabTrackCue = 0;
};

class OscOutput {
public:
    OscOutput(IOscTransport& transport, const OscConfig& cfg = {}) : tx_(transport), cfg_(cfg) {}

    // Call periodically from the scheduler thread.
    void update(const clock::Timeline& tl, int64_t nowNs);
    void onEvent(const net::Event& ev);
    // Metadata arrives from the tracklist/metadata layer, not from packets.
    void onTrackLoaded(uint8_t deck, const std::string& title, const std::string& artist, uint32_t rekordboxId);

    const OutputStatus& status() const { return status_; }
    void setConfig(const OscConfig& c) { cfg_ = c; }

private:
    void send(const std::vector<OscMessage>& msgs);
    bool sendShunt() const { return cfg_.profile == OscProfile::Generic || cfg_.shuntMessages; }

    IOscTransport& tx_;
    OscConfig cfg_;
    OutputStatus status_;
    clock::PublishPolicy policy_;
    int64_t lastBeat_ = INT64_MIN;
    int lastPlaying_ = -1;
    std::map<uint8_t, uint8_t> onAir_;
};

} // namespace shunt::out
