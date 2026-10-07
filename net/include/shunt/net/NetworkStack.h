// Glue: sockets -> parsers -> device table / claim machine / master tracker -> SPSC queue.
// Single receive thread for now (poll over the three sockets); ES-01 section 8
// thread split and priorities are a later step.
#pragma once
#include "shunt/net/Socket.h"
#include "shunt/net/Packets.h"
#include "shunt/net/DeviceTable.h"
#include "shunt/net/ClaimMachine.h"
#include "shunt/net/MasterTracker.h"
#include "shunt/net/SpscQueue.h"
#include "shunt/net/Pcapng.h"
#include <memory>
#include <vector>
#include <mutex>

namespace shunt::net {

struct StackConfig {
    Mode mode = Mode::Follow;
    uint32_t address = 0, netmask = 0, broadcast = 0xffffffff;
    std::array<uint8_t, 6> mac{};
    uint8_t leadNumber = 1;
    bool filterSubnet = true;
    std::string capturePath;         // optional pcapng recording
};

struct Counters {
    uint64_t datagrams = 0, parsed = 0, parseErrors = 0, filtered = 0, defendsSeen = 0, sent = 0;
    uint64_t perType[256] = {};
};

class NetworkStack : public ISender, public IClock {
public:
    NetworkStack(ISocketFactory& factory, const StackConfig& cfg);
    bool start();
    void stop();
    // Wait up to timeoutMs for datagrams, receive and process them, run timers.
    void poll(int timeoutMs);
    // Feed a datagram directly (replay and tests); returns true if it produced an event.
    bool feed(uint16_t port, const uint8_t* data, size_t len, int64_t recvNs, uint32_t srcIp);
    void runTimers(int64_t nowNs);
    bool pop(Event& out) { return queue_.pop(out); }

    // ISender / IClock
    bool send(uint16_t dstPort, uint32_t dstIp, const uint8_t* data, size_t len) override;
    int64_t nowNs() override { return factory_.nowNs(); }

    const DeviceTable& devices() const { return devices_; }
    const MasterTracker& master() const { return master_; }
    const ClaimMachine& claim() const { return claim_; }
    const Counters& counters() const { return counters_; }
    const StackConfig& config() const { return cfg_; }
    const char* timestampSource() const;

private:
    void enqueue(const Event& ev);
    ISocketFactory& factory_;
    StackConfig cfg_;
    DeviceTable devices_;
    MasterTracker master_;
    ClaimMachine claim_;
    SpscQueue<Event, 4096> queue_;
    std::unique_ptr<IUdpSocket> announce_, beat_, status_;
    std::vector<IUdpSocket*> open_;
    Counters counters_;
    PcapngWriter capture_;
    int64_t lastExpiryNs_ = 0;
    bool running_ = false;
};

} // namespace shunt::net
