// Minimal pcapng writer and reader (ES-01 section 9). No libpcap.
// Writes Ethernet + IPv4 + UDP framed packets; reads Ethernet, raw IPv4 and
// Linux cooked captures.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <optional>

namespace shunt::net {

struct CapturedDatagram {
    int64_t timestampNs = 0;     // capture clock (wall) ns
    uint32_t srcIp = 0, dstIp = 0;
    uint16_t srcPort = 0, dstPort = 0;
    std::vector<uint8_t> payload;
};

class PcapngWriter {
public:
    ~PcapngWriter();
    bool open(const std::string& path);
    bool write(const CapturedDatagram& d);
    void close();
    bool isOpen() const { return f_ != nullptr; }
private:
    FILE* f_ = nullptr;
};

class PcapngReader {
public:
    bool open(const std::string& path);
    // Returns the next UDP datagram, skipping anything that is not UDP/IPv4.
    std::optional<CapturedDatagram> next();
    static std::vector<CapturedDatagram> readAll(const std::string& path);
private:
    std::vector<uint8_t> file_;
    size_t pos_ = 0;
    std::vector<uint16_t> linkTypes_;
    std::vector<uint64_t> tsDivisors_;     // ticks per second per interface
};

} // namespace shunt::net
