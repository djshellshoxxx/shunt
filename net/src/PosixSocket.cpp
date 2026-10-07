#include "shunt/net/Socket.h"
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <cstring>
#include <cerrno>
#ifdef __linux__
#include <sys/ioctl.h>
#include <linux/if_packet.h>
#endif

namespace shunt::net {

int64_t monotonicNowNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

std::string ipToString(uint32_t ip) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return buf;
}

uint32_t parseIp(const std::string& s) {
    in_addr a{};
    if (inet_pton(AF_INET, s.c_str(), &a) != 1) return 0;
    return ntohl(a.s_addr);
}

namespace {

class PosixUdpSocket : public IUdpSocket {
public:
    ~PosixUdpSocket() override { if (fd_ >= 0) ::close(fd_); }

    bool bind(uint16_t port) override {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) return false;
        int one = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
        setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
        setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
        int rcv = 256 * 1024;
        setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
#ifdef SO_TIMESTAMPNS
        if (setsockopt(fd_, SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof one) == 0) tsSource_ = "SO_TIMESTAMPNS";
#endif
        fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL, 0) | O_NONBLOCK);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) return false;
        port_ = port;
        // Offset to convert realtime kernel stamps to the monotonic clock.
        timespec rt{};
        clock_gettime(CLOCK_REALTIME, &rt);
        rtToMono_ = monotonicNowNs() - (int64_t(rt.tv_sec) * 1'000'000'000LL + rt.tv_nsec);
        return true;
    }

    bool receive(Datagram& out) override {
        sockaddr_in from{};
        iovec iov{out.data, sizeof out.data};
        alignas(8) char control[256];
        msghdr msg{};
        msg.msg_name = &from;
        msg.msg_namelen = sizeof from;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof control;
        const ssize_t n = recvmsg(fd_, &msg, 0);
        const int64_t fallback = monotonicNowNs();
        if (n < 0) return false;
        out.len = size_t(n);
        out.srcIp = ntohl(from.sin_addr.s_addr);
        out.srcPort = ntohs(from.sin_port);
        out.dstPort = port_;
        out.recvNs = fallback;
#ifdef SO_TIMESTAMPNS
        for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPNS) {
                timespec ts{};
                std::memcpy(&ts, CMSG_DATA(c), sizeof ts);
                out.recvNs = int64_t(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec + rtToMono_;
            }
        }
#endif
        return true;
    }

    bool send(const uint8_t* data, size_t len, uint32_t dstIp, uint16_t dstPort) override {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = htonl(dstIp);
        to.sin_port = htons(dstPort);
        return sendto(fd_, data, len, 0, reinterpret_cast<sockaddr*>(&to), sizeof to) == ssize_t(len);
    }

    uint16_t port() const override { return port_; }
    const char* timestampSource() const override { return tsSource_; }
    int fd() const { return fd_; }

private:
    int fd_ = -1;
    uint16_t port_ = 0;
    int64_t rtToMono_ = 0;
    const char* tsSource_ = "steady_clock";
};

class PosixFactory : public ISocketFactory {
public:
    std::unique_ptr<IUdpSocket> create() override { return std::make_unique<PosixUdpSocket>(); }
    bool waitAny(const std::vector<IUdpSocket*>& sockets, int timeoutMs) override {
        std::vector<pollfd> fds;
        for (auto* s : sockets) fds.push_back(pollfd{static_cast<PosixUdpSocket*>(s)->fd(), POLLIN, 0});
        return ::poll(fds.data(), fds.size(), timeoutMs) > 0;
    }
    int64_t nowNs() override { return monotonicNowNs(); }
};

} // namespace

std::unique_ptr<ISocketFactory> makePosixSocketFactory() { return std::make_unique<PosixFactory>(); }

std::vector<InterfaceInfo> enumerateInterfaces() {
    std::vector<InterfaceInfo> out;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
        if (!(a->ifa_flags & IFF_UP)) continue;
        InterfaceInfo info;
        info.name = a->ifa_name;
        info.address = ntohl(reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr.s_addr);
        if (a->ifa_netmask) info.netmask = ntohl(reinterpret_cast<sockaddr_in*>(a->ifa_netmask)->sin_addr.s_addr);
        if ((a->ifa_flags & IFF_BROADCAST) && a->ifa_broadaddr)
            info.broadcast = ntohl(reinterpret_cast<sockaddr_in*>(a->ifa_broadaddr)->sin_addr.s_addr);
        else
            info.broadcast = info.address | ~info.netmask;
        out.push_back(info);
    }
#ifdef __linux__
    for (ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_PACKET) continue;
        auto* ll = reinterpret_cast<sockaddr_ll*>(a->ifa_addr);
        for (auto& info : out)
            if (info.name == a->ifa_name && ll->sll_halen == 6) std::memcpy(info.mac.data(), ll->sll_addr, 6);
    }
#endif
    freeifaddrs(list);
    return out;
}

} // namespace shunt::net
