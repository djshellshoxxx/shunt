#include "shunt/net/NetworkStack.h"
#include <chrono>

namespace shunt::net {

namespace {
ClaimMachine::Config claimConfig(const StackConfig& c) {
    ClaimMachine::Config k;
    k.mode = c.mode;
    k.mac = c.mac;
    k.ip = c.address;
    k.broadcast = c.broadcast;
    k.leadNumber = c.leadNumber;
    return k;
}
}

NetworkStack::NetworkStack(ISocketFactory& factory, const StackConfig& cfg)
    : factory_(factory), cfg_(cfg), claim_(claimConfig(cfg), *this, *this, devices_) {
    master_.setPassive(cfg.mode == Mode::Passive);
}

bool NetworkStack::start() {
    open_.clear();
    beat_ = factory_.create();
    if (!beat_->bind(kBeatPort)) return false;
    open_.push_back(beat_.get());
    if (cfg_.mode != Mode::Passive) {
        announce_ = factory_.create();
        status_ = factory_.create();
        if (!announce_->bind(kAnnouncePort) || !status_->bind(kStatusPort)) return false;
        open_.push_back(announce_.get());
        open_.push_back(status_.get());
    }
    if (!cfg_.capturePath.empty()) capture_.open(cfg_.capturePath);
    running_ = true;
    lastExpiryNs_ = nowNs();
    claim_.start();
    return true;
}

void NetworkStack::stop() {
    claim_.stop();
    running_ = false;
    open_.clear();
    announce_.reset();
    beat_.reset();
    status_.reset();
    capture_.close();
}

bool NetworkStack::send(uint16_t dstPort, uint32_t dstIp, const uint8_t* data, size_t len) {
    if (cfg_.mode == Mode::Passive) return false;      // N6 / N-T6: never send in Passive mode
    IUdpSocket* s = dstPort == kAnnouncePort ? announce_.get() : dstPort == kBeatPort ? beat_.get() : status_.get();
    if (!s) return false;
    ++counters_.sent;
    return s->send(data, len, dstIp, dstPort);
}

const char* NetworkStack::timestampSource() const {
    return beat_ ? beat_->timestampSource() : "none";
}

void NetworkStack::enqueue(const Event& ev) { queue_.push(ev); }

bool NetworkStack::feed(uint16_t port, const uint8_t* data, size_t len, int64_t recvNs, uint32_t srcIp) {
    ++counters_.datagrams;
    if (cfg_.filterSubnet && cfg_.netmask != 0 && srcIp != 0 &&
        (srcIp & cfg_.netmask) != (cfg_.address & cfg_.netmask)) {
        ++counters_.filtered;
        return false;
    }
    if (capture_.isOpen()) {
        CapturedDatagram d;
        d.timestampNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        d.srcIp = srcIp; d.dstIp = cfg_.broadcast; d.srcPort = port; d.dstPort = port;
        d.payload.assign(data, data + len);
        capture_.write(d);
    }
    auto ev = parseForPort(port, data, len, recvNs, srcIp);
    if (!ev) { ++counters_.parseErrors; return false; }
    ++counters_.parsed;
    if (len > 0x0a) ++counters_.perType[data[0x0a]];

    switch (ev->type) {
    case EventType::KeepAlive:
        if (ev->keepAlive.mac == cfg_.mac && cfg_.mac != std::array<uint8_t, 6>{}) return false; // our own
        if (auto dev = devices_.onKeepAlive(*ev)) enqueue(*dev);
        master_.setMixerPresent(devices_.mixerPresent());
        enqueue(*ev);
        break;
    case EventType::Claim:
        if (ev->claim.packetType == 0x08) ++counters_.defendsSeen;
        claim_.onAnnounce(*ev);
        enqueue(*ev);
        break;
    case EventType::PlayerStatus:
    case EventType::MixerStatus:
        if (auto dev = devices_.onStatus(*ev)) enqueue(*dev);
        if (auto mc = master_.onEvent(*ev)) enqueue(*mc);
        enqueue(*ev);
        break;
    case EventType::Beat:
        devices_.onBeat(ev->device, recvNs);
        if (auto mc = master_.onEvent(*ev)) enqueue(*mc);
        enqueue(*ev);
        break;
    default:
        enqueue(*ev);
        break;
    }
    return true;
}

void NetworkStack::runTimers(int64_t now) {
    claim_.tick();
    if (auto mc = master_.tick(now)) enqueue(*mc);
    if (now - lastExpiryNs_ > 1'000'000'000LL) {
        lastExpiryNs_ = now;
        for (auto& e : devices_.expire(now)) enqueue(e);
        master_.setMixerPresent(devices_.mixerPresent());
    }
}

void NetworkStack::poll(int timeoutMs) {
    if (!running_) return;
    if (factory_.waitAny(open_, timeoutMs)) {
        Datagram d;
        for (auto* s : open_) {
            while (s->receive(d)) feed(d.dstPort, d.data, d.len, d.recvNs, d.srcIp);
        }
    }
    runTimers(nowNs());
}

} // namespace shunt::net
