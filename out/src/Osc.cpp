#include "shunt/out/Osc.h"
#include "shunt/net/Socket.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace shunt::out {

namespace {
void pad(std::vector<uint8_t>& v) { while (v.size() % 4) v.push_back(0); }
void putStr(std::vector<uint8_t>& v, const std::string& s) { v.insert(v.end(), s.begin(), s.end()); v.push_back(0); pad(v); }
void putU32(std::vector<uint8_t>& v, uint32_t x) { for (int sh = 24; sh >= 0; sh -= 8) v.push_back(uint8_t(x >> sh)); }
std::string fmt(double v) { char b[32]; std::snprintf(b, sizeof b, "%.2f", v); return b; }
}

std::vector<uint8_t> oscMessage(const std::string& address, const std::vector<OscArg>& args) {
    std::vector<uint8_t> v;
    putStr(v, address);
    std::string tags = ",";
    for (auto& a : args) tags += a.type;
    putStr(v, tags);
    for (auto& a : args) {
        if (a.type == 'i') putU32(v, uint32_t(a.i));
        else if (a.type == 'f') { uint32_t u; std::memcpy(&u, &a.f, 4); putU32(v, u); }
        else putStr(v, a.s);
    }
    return v;
}

bool oscParse(const uint8_t* d, size_t n, OscMessage& out) {
    auto str = [&](size_t& p, std::string& o) {
        size_t e = p;
        while (e < n && d[e]) ++e;
        if (e >= n) return false;
        o.assign(reinterpret_cast<const char*>(d + p), e - p);
        p = (e + 4) & ~size_t(3);      // string plus terminator padded to 4
        return p <= n;
    };
    size_t p = 0;
    std::string tags;
    if (n < 4 || d[0] != '/') return false;
    if (!str(p, out.address) || !str(p, tags) || tags.empty() || tags[0] != ',') return false;
    out.args.clear();
    for (size_t i = 1; i < tags.size(); ++i) {
        switch (tags[i]) {
        case 'i': {
            if (p + 4 > n) return false;
            out.args.push_back(OscArg::Int(int32_t((uint32_t(d[p]) << 24) | (uint32_t(d[p + 1]) << 16) | (uint32_t(d[p + 2]) << 8) | d[p + 3])));
            p += 4;
            break;
        }
        case 'f': {
            if (p + 4 > n) return false;
            const uint32_t u = (uint32_t(d[p]) << 24) | (uint32_t(d[p + 1]) << 16) | (uint32_t(d[p + 2]) << 8) | d[p + 3];
            float f; std::memcpy(&f, &u, 4);
            out.args.push_back(OscArg::Float(f));
            p += 4;
            break;
        }
        case 's': { std::string v; if (!str(p, v)) return false; out.args.push_back(OscArg::Str(v)); break; }
        case 'T': out.args.push_back(OscArg::Int(1)); break;
        case 'F': out.args.push_back(OscArg::Int(0)); break;
        default: return false;
        }
    }
    return true;
}

OscInput::~OscInput() { close(); }
void OscInput::close() { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }

bool OscInput::open(uint16_t port, std::string& error) {
    close();
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) { error = "socket failed"; return false; }
    int on = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) { error = "cannot bind UDP " + std::to_string(port); close(); return false; }
    ::fcntl(fd_, F_SETFL, ::fcntl(fd_, F_GETFL, 0) | O_NONBLOCK);
    return true;
}

void OscInput::poll(const std::function<void(const OscMessage&)>& fn) {
    if (fd_ < 0) return;
    uint8_t buf[1500];
    for (int i = 0; i < 32; ++i) {
        const ssize_t r = ::recv(fd_, buf, sizeof buf, 0);
        if (r <= 0) break;
        OscMessage m;
        if (oscParse(buf, size_t(r), m)) fn(m);
    }
}

const char* oscProfileName(OscProfile p) {
    switch (p) {
    case OscProfile::Resolume: return "resolume";
    case OscProfile::GrandMA3: return "grandma3";
    case OscProfile::MagicQ: return "magicq";
    case OscProfile::QLab: return "qlab";
    default: return "generic";
    }
}

OscProfile oscProfileFromName(const std::string& s) {
    if (s == "resolume") return OscProfile::Resolume;
    if (s == "grandma3") return OscProfile::GrandMA3;
    if (s == "magicq") return OscProfile::MagicQ;
    if (s == "qlab") return OscProfile::QLab;
    return OscProfile::Generic;
}

OscOutput::~OscOutput() { if (fd_ >= 0) ::close(fd_); }

void OscOutput::configure(const OscSettings& s) {
    const bool socketChanged = s.host != s_.host || s.port != s_.port || !s.enabled;
    s_ = s;
    if (socketChanged && fd_ >= 0) { ::close(fd_); fd_ = -1; }
    error_.clear();
    haveBeat_ = false;
    policy_ = clock::PublishPolicy{};
    if (s_.enabled && !sink_ && fd_ < 0) openSocket();
}

bool OscOutput::openSocket() {
    ip_ = net::parseIp(s_.host);
    if (!ip_) { error_ = "invalid host"; return false; }
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) { error_ = "socket failed"; return false; }
    int on = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    return true;
}

void OscOutput::emit(const std::string& address, const std::vector<OscArg>& args) {
    if (!s_.enabled) return;
    auto bytes = oscMessage(address, args);
    if (sink_) { sink_(bytes); ++sent_; return; }
    if (fd_ < 0 && !openSocket()) return;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(s_.port);
    to.sin_addr.s_addr = htonl(ip_);
    if (::sendto(fd_, bytes.data(), bytes.size(), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&to), sizeof to) < 0) error_ = "send failed";
    else { error_.clear(); ++sent_; }
}

void OscOutput::emitTempo(double bpm) {
    switch (s_.profile) {
    case OscProfile::Resolume:
        emit("/composition/tempocontroller/tempo", {OscArg::Float(float(std::clamp((bpm - 20.0) / 480.0, 0.0, 1.0)))});
        break;
    case OscProfile::GrandMA3:
        emit("/gma3/cmd", {OscArg::Str("Master 3.1 At BPM " + fmt(bpm))});
        break;
    case OscProfile::MagicQ:
        emit("/bpm", {OscArg::Float(float(bpm))});
        emit("/shunt/bpm", {OscArg::Float(float(bpm))});
        break;
    case OscProfile::QLab:
    case OscProfile::Generic:
        emit("/shunt/bpm", {OscArg::Float(float(bpm))});
        break;
    }
}

void OscOutput::emitBeat(const clock::Timeline& tl, int64_t beat) {
    const int inBar = tl.barKnown ? int(((int64_t(tl.beatInBar - 1) + (beat - tl.beatIndex)) % 4 + 4) % 4) + 1 : 0;
    if (s_.profile == OscProfile::Resolume) {
        if (tl.barKnown && inBar == 1) emit("/composition/tempocontroller/resync", {OscArg::Int(1)});
        return;
    }
    if (s_.profile == OscProfile::GrandMA3 || s_.profile == OscProfile::QLab) return;
    emit("/shunt/beat", {OscArg::Int(inBar)});
    if (tl.barKnown && inBar == 1) emit("/shunt/bar", {OscArg::Int(1)});
}

void OscOutput::poll(int64_t nowNs, const clock::Timeline& tl) {
    if (!s_.enabled) return;
    if (tl.playing != lastPlaying_) {
        lastPlaying_ = tl.playing;
        if (s_.profile == OscProfile::Generic || s_.profile == OscProfile::MagicQ) emit("/shunt/transport", {OscArg::Int(tl.playing ? 1 : 0)});
    }
    if (tl.bpm <= 0) { haveBeat_ = false; return; }
    if (policy_.shouldPublishTempo(tl, nowNs)) emitTempo(tl.bpm);
    if (!haveBeat_ || tl.resetSequence != lastReset_) {
        lastReset_ = tl.resetSequence;
        const double pos = double(tl.beatIndex) + double(nowNs - tl.beatOriginNs) / tl.periodNs();
        nextBeat_ = int64_t(std::floor(pos)) + 1;
        haveBeat_ = true;
    }
    int guard = 0;
    while (guard++ < 8 && nowNs >= tl.timeOfBeat(nextBeat_)) {
        if (nowNs - tl.timeOfBeat(nextBeat_) < int64_t(tl.periodNs())) emitBeat(tl, nextBeat_);
        ++nextBeat_;
    }
}

void OscOutput::onMasterChanged(int device) {
    switch (s_.profile) {
    case OscProfile::QLab: emit("/cue/" + std::to_string(s_.qlabMasterCue) + "/start", {}); break;
    case OscProfile::Resolume:
    case OscProfile::GrandMA3: break;
    default: emit("/shunt/master", {OscArg::Int(device)}); break;
    }
}

void OscOutput::onTrackLoaded(int deck, uint32_t id, const std::string& title, const std::string& artist) {
    switch (s_.profile) {
    case OscProfile::QLab: emit("/cue/" + std::to_string(s_.qlabTrackCue) + "/start", {}); break;
    case OscProfile::Resolume:
    case OscProfile::GrandMA3: break;
    default:
        emit("/shunt/track", {OscArg::Str(title), OscArg::Str(artist), OscArg::Int(deck)});
        emit("/shunt/track/id", {OscArg::Int(int32_t(id))});
        break;
    }
}

void OscOutput::onOnAir(int deck, bool on) {
    if (s_.profile == OscProfile::Generic || s_.profile == OscProfile::MagicQ) emit("/shunt/onair", {OscArg::Int(deck), OscArg::Int(on ? 1 : 0)});
}

} // namespace shunt::out
