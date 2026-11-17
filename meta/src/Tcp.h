// Minimal portable TCP client (POSIX and WinSock) with per-step timeouts and a
// stop flag polled in short slices so a worker can be joined promptly.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace shunt::meta::detail {

class TcpConn {
public:
    TcpConn() = default;
    ~TcpConn() { close(); }
    TcpConn(const TcpConn&) = delete;
    TcpConn& operator=(const TcpConn&) = delete;

    // ip in host byte order. Each call waits at most timeoutMs.
    bool connect(uint32_t ip, uint16_t port, int timeoutMs, const std::atomic<bool>* stop);
    bool sendAll(const uint8_t* data, size_t len, int timeoutMs, const std::atomic<bool>* stop);
    bool recvAll(uint8_t* data, size_t len, int timeoutMs, const std::atomic<bool>* stop);
    void close();
    bool isOpen() const { return fd_ != kInvalid; }

private:
    enum class Wait { Ready, Timeout, Stopped, Error };
    Wait wait(bool forWrite, int timeoutMs, const std::atomic<bool>* stop);
#ifdef _WIN32
    static constexpr uintptr_t kInvalid = ~uintptr_t(0);
    uintptr_t fd_ = kInvalid;
#else
    static constexpr int kInvalid = -1;
    int fd_ = kInvalid;
#endif
};

// Initialises WinSock once per process; no-op elsewhere.
void ensureSocketsInitialised();

} // namespace shunt::meta::detail
