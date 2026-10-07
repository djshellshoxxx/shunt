// Socket abstraction (ES-01 section 3). Platform implementations live behind
// ISocketFactory so the stack above runs unchanged on mocks and replays.
#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>
#include <array>

namespace shunt::net {

struct Datagram {
    uint32_t srcIp = 0;        // host byte order
    uint16_t srcPort = 0;
    uint16_t dstPort = 0;
    int64_t recvNs = 0;        // monotonic
    size_t len = 0;
    uint8_t data[2048];
};

class IUdpSocket {
public:
    virtual ~IUdpSocket() = default;
    // Bind 0.0.0.0:port with SO_REUSEADDR (+SO_REUSEPORT where available) and SO_BROADCAST.
    virtual bool bind(uint16_t port) = 0;
    // Non-blocking receive; returns true when a datagram was read.
    virtual bool receive(Datagram& out) = 0;
    virtual bool send(const uint8_t* data, size_t len, uint32_t dstIp, uint16_t dstPort) = 0;
    virtual uint16_t port() const = 0;
    virtual const char* timestampSource() const = 0;
};

class ISocketFactory {
public:
    virtual ~ISocketFactory() = default;
    virtual std::unique_ptr<IUdpSocket> create() = 0;
    // Block until any socket is readable or timeout. Returns false on timeout.
    virtual bool waitAny(const std::vector<IUdpSocket*>& sockets, int timeoutMs) = 0;
    virtual int64_t nowNs() = 0;
};

struct InterfaceInfo {
    std::string name;
    uint32_t address = 0, netmask = 0, broadcast = 0;
    std::array<uint8_t, 6> mac{};
};

std::string ipToString(uint32_t ip);
uint32_t parseIp(const std::string& s);   // 0 on failure

// POSIX implementation (Linux and macOS).
std::unique_ptr<ISocketFactory> makePosixSocketFactory();
std::vector<InterfaceInfo> enumerateInterfaces();
int64_t monotonicNowNs();

} // namespace shunt::net
