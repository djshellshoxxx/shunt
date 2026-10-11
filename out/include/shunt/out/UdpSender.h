// Minimal portable UDP sender/receiver for OSC (POSIX and WinSock).
#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>

namespace shunt::out {

class UdpEndpoint {
public:
    UdpEndpoint();
    ~UdpEndpoint();
    UdpEndpoint(const UdpEndpoint&) = delete;
    UdpEndpoint& operator=(const UdpEndpoint&) = delete;

    bool openSender();                         // ephemeral port, broadcast allowed
    bool bindReceiver(uint16_t port);          // 0.0.0.0:port, non-blocking
    bool sendTo(const std::string& host, uint16_t port, const uint8_t* data, size_t len);
    // Waits up to timeoutMs; returns bytes read or -1.
    int receive(uint8_t* buf, size_t cap, int timeoutMs, uint32_t* srcIp = nullptr);
    void close();
    bool isOpen() const;
    std::string lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace shunt::out
