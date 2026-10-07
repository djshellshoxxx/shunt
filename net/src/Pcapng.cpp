#include "shunt/net/Pcapng.h"
#include <cstring>

namespace shunt::net {

namespace {

void le16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
void le32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
void be16v(std::vector<uint8_t>& b, uint16_t v) { b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); }
void be32v(std::vector<uint8_t>& b, uint32_t v) { for (int i = 3; i >= 0; --i) b.push_back(uint8_t(v >> (8 * i))); }
uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
uint16_t rbe16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
uint32_t rbe32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

uint16_t ipChecksum(const uint8_t* d, size_t n) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < n; i += 2) sum += (d[i] << 8) | d[i + 1];
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return uint16_t(~sum);
}

void writeBlock(FILE* f, uint32_t type, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> blk;
    const uint32_t total = uint32_t(12 + ((body.size() + 3) & ~size_t(3)));
    le32(blk, type);
    le32(blk, total);
    blk.insert(blk.end(), body.begin(), body.end());
    while (blk.size() % 4) blk.push_back(0);
    le32(blk, total);
    fwrite(blk.data(), 1, blk.size(), f);
}

constexpr uint16_t kLinkEthernet = 1, kLinkRawIp = 101, kLinkLinuxSll = 113, kLinkIpv4 = 228;

} // namespace

PcapngWriter::~PcapngWriter() { close(); }

bool PcapngWriter::open(const std::string& path) {
    f_ = fopen(path.c_str(), "wb");
    if (!f_) return false;
    std::vector<uint8_t> shb;
    le32(shb, 0x1A2B3C4D);
    le16(shb, 1); le16(shb, 0);
    for (int i = 0; i < 8; ++i) shb.push_back(0xff);   // section length unknown
    writeBlock(f_, 0x0A0D0D0A, shb);
    std::vector<uint8_t> idb;
    le16(idb, kLinkEthernet); le16(idb, 0);
    le32(idb, 65535);
    // if_tsresol option: 9 (nanoseconds)
    le16(idb, 9); le16(idb, 1); idb.push_back(9); idb.push_back(0); idb.push_back(0); idb.push_back(0);
    le16(idb, 0); le16(idb, 0);
    writeBlock(f_, 0x00000001, idb);
    return true;
}

bool PcapngWriter::write(const CapturedDatagram& d) {
    if (!f_) return false;
    std::vector<uint8_t> frame;
    for (int i = 0; i < 6; ++i) frame.push_back(0xff);
    for (int i = 0; i < 6; ++i) frame.push_back(0x02);
    be16v(frame, 0x0800);
    const size_t ipStart = frame.size();
    const uint16_t ipLen = uint16_t(20 + 8 + d.payload.size());
    frame.push_back(0x45); frame.push_back(0);
    be16v(frame, ipLen);
    be16v(frame, 0); be16v(frame, 0);
    frame.push_back(64); frame.push_back(17);
    be16v(frame, 0);
    be32v(frame, d.srcIp); be32v(frame, d.dstIp);
    const uint16_t csum = ipChecksum(frame.data() + ipStart, 20);
    frame[ipStart + 10] = uint8_t(csum >> 8); frame[ipStart + 11] = uint8_t(csum);
    be16v(frame, d.srcPort); be16v(frame, d.dstPort);
    be16v(frame, uint16_t(8 + d.payload.size())); be16v(frame, 0);
    frame.insert(frame.end(), d.payload.begin(), d.payload.end());

    std::vector<uint8_t> epb;
    le32(epb, 0);
    const uint64_t ts = uint64_t(d.timestampNs);
    le32(epb, uint32_t(ts >> 32)); le32(epb, uint32_t(ts));
    le32(epb, uint32_t(frame.size())); le32(epb, uint32_t(frame.size()));
    epb.insert(epb.end(), frame.begin(), frame.end());
    while (epb.size() % 4) epb.push_back(0);
    writeBlock(f_, 0x00000006, epb);
    return true;
}

void PcapngWriter::close() {
    if (f_) { fclose(f_); f_ = nullptr; }
}

bool PcapngReader::open(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) file_.insert(file_.end(), buf, buf + n);
    fclose(f);
    pos_ = 0;
    linkTypes_.clear();
    tsDivisors_.clear();
    return file_.size() >= 12;
}

std::optional<CapturedDatagram> PcapngReader::next() {
    while (pos_ + 12 <= file_.size()) {
        const uint8_t* b = file_.data() + pos_;
        const uint32_t type = rd32(b);
        const uint32_t len = rd32(b + 4);
        if (len < 12 || pos_ + len > file_.size()) return std::nullopt;
        const uint8_t* body = b + 8;
        const size_t bodyLen = len - 12;
        pos_ += len;
        if (type == 0x0A0D0D0A) {
            linkTypes_.clear();
            tsDivisors_.clear();
            continue;
        }
        if (type == 0x00000001 && bodyLen >= 8) {
            linkTypes_.push_back(rd16(body));
            uint64_t div = 1'000'000;
            size_t o = 8;
            while (o + 4 <= bodyLen) {
                const uint16_t code = rd16(body + o), olen = rd16(body + o + 2);
                if (code == 0) break;
                if (code == 9 && olen >= 1) {
                    const uint8_t r = body[o + 4];
                    if (r & 0x80) div = uint64_t(1) << (r & 0x7f);
                    else { div = 1; for (int i = 0; i < r; ++i) div *= 10; }
                }
                o += 4 + ((olen + 3) & ~3u);
            }
            tsDivisors_.push_back(div);
            continue;
        }
        if (type != 0x00000006 || bodyLen < 20) continue;
        const uint32_t iface = rd32(body);
        const uint64_t ts = (uint64_t(rd32(body + 4)) << 32) | rd32(body + 8);
        const uint32_t capLen = rd32(body + 12);
        if (20 + capLen > bodyLen) continue;
        const uint8_t* pkt = body + 20;
        const uint16_t link = iface < linkTypes_.size() ? linkTypes_[iface] : kLinkEthernet;
        const uint64_t div = iface < tsDivisors_.size() ? tsDivisors_[iface] : 1'000'000;
        size_t off = 0;
        if (link == kLinkEthernet) {
            if (capLen < 14 || rbe16(pkt + 12) != 0x0800) continue;
            off = 14;
        } else if (link == kLinkLinuxSll) {
            if (capLen < 16 || rbe16(pkt + 14) != 0x0800) continue;
            off = 16;
        } else if (link == kLinkRawIp || link == kLinkIpv4) {
            off = 0;
        } else {
            continue;
        }
        if (off + 20 > capLen) continue;
        const uint8_t* ip = pkt + off;
        if ((ip[0] >> 4) != 4 || ip[9] != 17) continue;
        const size_t ihl = size_t(ip[0] & 0x0f) * 4;
        if (off + ihl + 8 > capLen) continue;
        const uint8_t* udp = ip + ihl;
        const size_t udpLen = rbe16(udp + 4);
        if (udpLen < 8 || off + ihl + udpLen > capLen) continue;
        CapturedDatagram d;
        d.timestampNs = (1'000'000'000ULL % div == 0) ? int64_t(ts * (1'000'000'000ULL / div)) : int64_t(double(ts) * (1e9 / double(div)));
        d.srcIp = rbe32(ip + 12);
        d.dstIp = rbe32(ip + 16);
        d.srcPort = rbe16(udp);
        d.dstPort = rbe16(udp + 2);
        d.payload.assign(udp + 8, udp + udpLen);
        return d;
    }
    return std::nullopt;
}

std::vector<CapturedDatagram> PcapngReader::readAll(const std::string& path) {
    std::vector<CapturedDatagram> out;
    PcapngReader r;
    if (!r.open(path)) return out;
    while (auto d = r.next()) out.push_back(std::move(*d));
    return out;
}

} // namespace shunt::net
