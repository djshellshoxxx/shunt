// In-process fake player dbserver on loopback (discovery port + dbserver port).
// Its codec is written independently of meta/src so the two cross-check each other.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fake {

#ifdef _WIN32
using Sock = SOCKET;
constexpr Sock kBad = INVALID_SOCKET;
inline void closeSock(Sock s) { closesocket(s); }
inline void initNet() { static bool ok = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }(); (void)ok; }
#else
using Sock = int;
constexpr Sock kBad = -1;
inline void closeSock(Sock s) { ::close(s); }
inline void initNet() {}
#endif

inline bool readable(Sock s, int ms) {
    fd_set r;
    FD_ZERO(&r);
    FD_SET(s, &r);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    return ::select(int(s) + 1, &r, nullptr, nullptr, &tv) > 0;
}

inline Sock listenLoopback(uint16_t& port) {
    Sock s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0x7f000001);
    a.sin_port = 0;
    ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
    ::listen(s, 8);
    socklen_t len = sizeof a;
    ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    port = ntohs(a.sin_port);
    return s;
}

enum class Mode {
    Normal, NotFound, Silent, BadMagic, WrongTransaction, HugeString, Garbage,
    CloseMidRender, DiscoveryPortZero, NoFooter,
};

struct Track { uint32_t id; std::string title, artist, album, genre, key, label; uint32_t bpm100, seconds; };

struct Wire {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(0x0f); b.push_back(v); }
    void u16(uint16_t v) { b.push_back(0x10); b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); }
    void raw32(uint32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s)); }
    void u32(uint32_t v) { b.push_back(0x11); raw32(v); }
    void str(const std::string& ascii) {   // test labels are ASCII or pre-encoded UTF-16BE below
        b.push_back(0x26); raw32(uint32_t(ascii.size() + 1));
        for (char c : ascii) { b.push_back(0); b.push_back(uint8_t(c)); }
        b.push_back(0); b.push_back(0);
    }
    void header(uint32_t tx, uint16_t type, uint8_t argc) {
        u32(0x872349ae); u32(tx); u16(type); u8(argc);
        b.push_back(0x14); raw32(12);
        for (int i = 0; i < 12; ++i) b.push_back(i < argc ? 0x06 : 0);
    }
};

struct InField { uint8_t tag = 0; uint32_t num = 0; };
struct InMsg { uint32_t tx = 0; uint16_t type = 0; std::vector<uint32_t> args; };

class Server {
public:
    explicit Server(Mode m = Mode::Normal) : mode(m) {
        initNet();
        disc_ = listenLoopback(discPort_);
        db_ = listenLoopback(dbPort_);
        thread_ = std::thread([this] { loop(); });
    }
    ~Server() {
        stop_ = true;
        thread_.join();
        closeSock(disc_);
        closeSock(db_);
    }
    uint16_t discoveryPort() const { return discPort_; }
    void add(const Track& t) { std::lock_guard<std::mutex> l(mu_); tracks_[t.id] = t; }

    Mode mode;
    std::atomic<int> discoveryHits{0}, sessions{0}, teardowns{0};
    std::atomic<uint32_t> greetingNumber{0}, dmst{0}, requestedId{0}, renderOffset{99}, renderCount{0};
    std::atomic<bool> badDiscoveryQuery{false};
    std::atomic<int> holdMs{3000};   // how long Silent mode keeps the connection open

private:
    void loop() {
        while (!stop_) {
            fd_set r;
            FD_ZERO(&r);
            FD_SET(disc_, &r);
            FD_SET(db_, &r);
            timeval tv{0, 20000};
            const int mx = int(disc_ > db_ ? disc_ : db_);
            if (::select(mx + 1, &r, nullptr, nullptr, &tv) <= 0) continue;
            if (FD_ISSET(disc_, &r)) { Sock c = ::accept(disc_, nullptr, nullptr); if (c != kBad) { discovery(c); closeSock(c); } }
            if (FD_ISSET(db_, &r)) { Sock c = ::accept(db_, nullptr, nullptr); if (c != kBad) { ++sessions; session(c); closeSock(c); } }
        }
    }

    bool recvN(Sock c, uint8_t* p, size_t n, int ms = 2000) {
        while (n) {
            if (!readable(c, ms)) return false;
            const int got = int(::recv(c, reinterpret_cast<char*>(p), int(n), 0));
            if (got <= 0) return false;
            p += got; n -= size_t(got);
        }
        return true;
    }
    void sendAll(Sock c, const std::vector<uint8_t>& b) {
        size_t off = 0;
        while (off < b.size()) {
            const int n = int(::send(c, reinterpret_cast<const char*>(b.data() + off), int(b.size() - off), 0));
            if (n <= 0) return;
            off += size_t(n);
        }
    }
    bool readField(Sock c, InField& f) {
        uint8_t t, v[4];
        if (!recvN(c, &t, 1)) return false;
        f.tag = t;
        size_t n = t == 0x0f ? 1 : t == 0x10 ? 2 : (t == 0x11 || t == 0x14 || t == 0x26) ? 4 : 0;
        if (!n || !recvN(c, v, n)) return false;
        f.num = 0;
        for (size_t i = 0; i < n; ++i) f.num = f.num << 8 | v[i];
        if (t == 0x14 || t == 0x26) {
            std::vector<uint8_t> skip(size_t(f.num) * (t == 0x26 ? 2 : 1));
            if (skip.size() > 4096 || (!skip.empty() && !recvN(c, skip.data(), skip.size()))) return false;
        }
        return true;
    }
    bool readMsg(Sock c, InMsg& m) {
        InField f;
        if (!readField(c, f) || f.tag != 0x11 || f.num != 0x872349ae) return false;
        if (!readField(c, f) || f.tag != 0x11) return false;
        m.tx = f.num;
        if (!readField(c, f) || f.tag != 0x10) return false;
        m.type = uint16_t(f.num);
        if (!readField(c, f) || f.tag != 0x0f || f.num > 12) return false;
        const uint32_t argc = f.num;
        if (!readField(c, f) || f.tag != 0x14 || f.num != 12) return false;
        m.args.clear();
        for (uint32_t i = 0; i < argc; ++i) { if (!readField(c, f)) return false; m.args.push_back(f.num); }
        return true;
    }

    void discovery(Sock c) {
        ++discoveryHits;
        uint8_t q[19];
        static const uint8_t expect[19] = {0, 0, 0, 0x0f, 'R', 'e', 'm', 'o', 't', 'e', 'D', 'B', 'S', 'e', 'r', 'v', 'e', 'r', 0};
        if (!recvN(c, q, 19) || std::memcmp(q, expect, 19) != 0) { badDiscoveryQuery = true; return; }
        const uint16_t p = mode == Mode::DiscoveryPortZero ? 0 : dbPort_;
        sendAll(c, {uint8_t(p >> 8), uint8_t(p)});
    }

    void item(Wire& w, uint32_t tx, uint32_t type, uint32_t num, const std::string& label) {
        w.header(tx, 0x4101, 12);
        w.u32(0); w.u32(num); w.u32(uint32_t(label.size() * 2 + 2)); w.str(label);
        w.u32(2); w.str(""); w.u32(type); w.u32(0); w.u32(0); w.u32(0); w.u32(0); w.u32(0);
    }

    void session(Sock c) {
        uint8_t pre[5];
        if (!recvN(c, pre, 5) || pre[0] != 0x11) return;
        if (mode == Mode::Garbage) { sendAll(c, {0x11, 0, 0, 0, 1, 0x99, 0x42, 0x00, 0xff, 0xff, 0xff, 0xff, 0x13}); return; }
        sendAll(c, {0x11, 0, 0, 0, 1});
        InMsg m;
        if (!readMsg(c, m) || m.type != 0x0000 || m.tx != 0xfffffffe || m.args.size() != 1) return;
        greetingNumber = m.args[0];
        if (mode == Mode::Silent) {
            for (int t = 0; t < holdMs && !stop_; t += 20) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return;
        }
        Wire w;
        if (mode == Mode::BadMagic) { w.u32(0x12345678); w.u32(0xfffffffe); w.u16(0x4000); sendAll(c, w.b); return; }
        w.header(0xfffffffe, 0x4000, 2); w.u32(0); w.u32(3);
        sendAll(c, w.b);

        if (!readMsg(c, m) || m.type != 0x2002 || m.args.size() != 2) return;
        dmst = m.args[0];
        requestedId = m.args[1];
        Track t{};
        bool found;
        { std::lock_guard<std::mutex> l(mu_); auto it = tracks_.find(m.args[1]); found = it != tracks_.end(); if (found) t = it->second; }
        if (mode == Mode::NotFound) found = false;
        const uint32_t count = 8;
        w.b.clear();
        w.header(mode == Mode::WrongTransaction ? m.tx + 7 : m.tx, 0x4000, 2); w.u32(0x2002); w.u32(found ? count : 0);
        sendAll(c, w.b);
        if (!found || mode == Mode::WrongTransaction) { waitTeardown(c); return; }

        if (!readMsg(c, m) || m.type != 0x3000 || m.args.size() != 6) return;
        renderOffset = m.args[1];
        renderCount = m.args[2];
        const uint32_t tx = m.tx;
        w.b.clear();
        w.header(tx, 0x4001, 0);
        item(w, tx, 0x04, t.id, t.title);
        item(w, tx, 0x07, 0, t.artist);
        if (mode == Mode::HugeString) {
            w.header(tx, 0x4101, 12); w.u32(0); w.u32(0); w.u32(0);
            w.b.push_back(0x26); w.raw32(0x7fffffff);
            sendAll(c, w.b);
            return;
        }
        if (mode == Mode::CloseMidRender) { sendAll(c, w.b); return; }
        item(w, tx, 0x02, 0, t.album);
        item(w, tx, 0x06, 0, t.genre);
        item(w, tx, 0x0f, 0, t.key);
        item(w, tx, 0x0d, t.bpm100, "");
        item(w, tx, 0x0b, t.seconds, "");
        item(w, tx, 0x0e, 0, t.label);
        if (mode != Mode::NoFooter) w.header(tx, 0x4201, 0);
        sendAll(c, w.b);
        if (mode == Mode::NoFooter) { waitTeardown(c); return; }
        waitTeardown(c);
    }

    void waitTeardown(Sock c) {
        InMsg m;
        if (readMsg(c, m) && m.type == 0x0100) ++teardowns;
    }

    Sock disc_ = kBad, db_ = kBad;
    uint16_t discPort_ = 0, dbPort_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex mu_;
    std::map<uint32_t, Track> tracks_;
};

} // namespace fake
