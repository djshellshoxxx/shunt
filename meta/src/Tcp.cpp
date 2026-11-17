#include "Tcp.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace shunt::meta::detail {

namespace {
constexpr int kSliceMs = 50;

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

#ifdef _WIN32
using Fd = SOCKET;
inline Fd toFd(uintptr_t v) { return Fd(v); }
inline bool wouldBlock() { int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
inline bool interrupted() { return WSAGetLastError() == WSAEINTR; }
#else
using Fd = int;
inline Fd toFd(int v) { return v; }
inline bool wouldBlock() { return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS; }
inline bool interrupted() { return errno == EINTR; }
#endif
} // namespace

void ensureSocketsInitialised() {
#ifdef _WIN32
    static const bool ok = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
    (void)ok;
#endif
}

TcpConn::Wait TcpConn::wait(bool forWrite, int timeoutMs, const std::atomic<bool>* stop) {
    const int64_t deadline = nowMs() + std::max(0, timeoutMs);
    for (;;) {
        if (stop && stop->load()) return Wait::Stopped;
        const int64_t left = deadline - nowMs();
        if (left <= 0) return Wait::Timeout;
        const int slice = int(std::min<int64_t>(left, kSliceMs));
#ifdef _WIN32
        fd_set rw, ex;
        FD_ZERO(&rw); FD_ZERO(&ex);
        FD_SET(toFd(fd_), &rw); FD_SET(toFd(fd_), &ex);
        timeval tv{slice / 1000, (slice % 1000) * 1000};
        const int r = ::select(0, forWrite ? nullptr : &rw, forWrite ? &rw : nullptr, &ex, &tv);
        if (r == SOCKET_ERROR) return Wait::Error;
        if (r == 0) continue;
        if (FD_ISSET(toFd(fd_), &ex)) return Wait::Error;
        return Wait::Ready;
#else
        pollfd p{fd_, short(forWrite ? POLLOUT : POLLIN), 0};
        const int r = ::poll(&p, 1, slice);
        if (r < 0) { if (interrupted()) continue; return Wait::Error; }
        if (r == 0) continue;
        if (p.revents & (POLLIN | POLLOUT)) return Wait::Ready;
        return Wait::Error;
#endif
    }
}

bool TcpConn::connect(uint32_t ip, uint16_t port, int timeoutMs, const std::atomic<bool>* stop) {
    close();
    ensureSocketsInitialised();
    Fd s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
    if (s == INVALID_SOCKET) return false;
    fd_ = uintptr_t(s);
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    if (s < 0) return false;
    fd_ = s;
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
#endif
    int nodelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof nodelay);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(ip);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0) return true;
    if (!wouldBlock()) { close(); return false; }
    if (wait(true, timeoutMs, stop) != Wait::Ready) { close(); return false; }
    int err = 0;
    socklen_t len = sizeof err;
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) != 0 || err != 0) {
        close();
        return false;
    }
    return true;
}

bool TcpConn::sendAll(const uint8_t* data, size_t len, int timeoutMs, const std::atomic<bool>* stop) {
    if (!isOpen()) return false;
    while (len > 0) {
        const int chunk = int(std::min<size_t>(len, 1 << 20));
#if defined(_WIN32)
        const int n = ::send(toFd(fd_), reinterpret_cast<const char*>(data), chunk, 0);
#elif defined(MSG_NOSIGNAL)
        const auto n = ::send(fd_, data, size_t(chunk), MSG_NOSIGNAL);
#else
        const auto n = ::send(fd_, data, size_t(chunk), 0);
#endif
        if (n > 0) { data += n; len -= size_t(n); continue; }
        if (n < 0 && interrupted()) continue;
        if (n < 0 && wouldBlock()) {
            if (wait(true, timeoutMs, stop) != Wait::Ready) return false;
            continue;
        }
        return false;
    }
    return true;
}

bool TcpConn::recvAll(uint8_t* data, size_t len, int timeoutMs, const std::atomic<bool>* stop) {
    if (!isOpen()) return false;
    while (len > 0) {
        const int chunk = int(std::min<size_t>(len, 1 << 20));
#ifdef _WIN32
        const int n = ::recv(toFd(fd_), reinterpret_cast<char*>(data), chunk, 0);
#else
        const auto n = ::recv(fd_, data, size_t(chunk), 0);
#endif
        if (n > 0) { data += n; len -= size_t(n); continue; }
        if (n == 0) return false;   // peer closed
        if (interrupted()) continue;
        if (wouldBlock()) {
            if (wait(false, timeoutMs, stop) != Wait::Ready) return false;
            continue;
        }
        return false;
    }
    return true;
}

void TcpConn::close() {
    if (fd_ == kInvalid) return;
#ifdef _WIN32
    ::closesocket(toFd(fd_));
#else
    ::close(fd_);
#endif
    fd_ = kInvalid;
}

} // namespace shunt::meta::detail
