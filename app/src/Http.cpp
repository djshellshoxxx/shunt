#include "shunt/app/Http.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <list>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>

namespace shunt::app {

// ---- SHA-1 / base64 / WebSocket framing -------------------------------------------------------

std::string sha1(const std::string& data) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string m = data;
    const uint64_t bits = uint64_t(data.size()) * 8;
    m += char(0x80);
    while (m.size() % 64 != 56) m += char(0);
    for (int i = 7; i >= 0; --i) m += char((bits >> (i * 8)) & 0xff);
    auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(uint8_t(m[off + 4 * i])) << 24) | (uint32_t(uint8_t(m[off + 4 * i + 1])) << 16) |
                   (uint32_t(uint8_t(m[off + 4 * i + 2])) << 8) | uint32_t(uint8_t(m[off + 4 * i + 3]));
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::string out;
    for (uint32_t v : h) for (int i = 3; i >= 0; --i) out += char((v >> (i * 8)) & 0xff);
    return out;
}

std::string base64(const std::string& d) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < d.size(); i += 3) {
        uint32_t v = uint32_t(uint8_t(d[i])) << 16;
        if (i + 1 < d.size()) v |= uint32_t(uint8_t(d[i + 1])) << 8;
        if (i + 2 < d.size()) v |= uint8_t(d[i + 2]);
        o += T[(v >> 18) & 63];
        o += T[(v >> 12) & 63];
        o += i + 1 < d.size() ? T[(v >> 6) & 63] : '=';
        o += i + 2 < d.size() ? T[v & 63] : '=';
    }
    return o;
}

std::string wsAcceptKey(const std::string& k) { return base64(sha1(k + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")); }

std::string wsEncodeText(const std::string& p) {
    std::string f;
    f += char(0x81);
    if (p.size() < 126) f += char(p.size());
    else if (p.size() < 65536) { f += char(126); f += char(p.size() >> 8); f += char(p.size() & 0xff); }
    else { f += char(127); for (int i = 7; i >= 0; --i) f += char((uint64_t(p.size()) >> (i * 8)) & 0xff); }
    return f + p;
}

int wsDecode(const std::string& b, int& opcode, std::string& payload) {
    if (b.size() < 2) return 0;
    const uint8_t b0 = uint8_t(b[0]), b1 = uint8_t(b[1]);
    if (!(b0 & 0x80)) return -1;                 // fragmentation not supported
    opcode = b0 & 0x0f;
    const bool masked = b1 & 0x80;
    uint64_t len = b1 & 0x7f;
    size_t pos = 2;
    if (len == 126) { if (b.size() < 4) return 0; len = (uint64_t(uint8_t(b[2])) << 8) | uint8_t(b[3]); pos = 4; }
    else if (len == 127) {
        if (b.size() < 10) return 0;
        len = 0;
        for (int i = 0; i < 8; ++i) len = (len << 8) | uint8_t(b[2 + i]);
        pos = 10;
    }
    if (len > (1u << 20)) return -1;
    uint8_t mask[4] = {0, 0, 0, 0};
    if (masked) { if (b.size() < pos + 4) return 0; std::memcpy(mask, b.data() + pos, 4); pos += 4; }
    if (b.size() < pos + len) return 0;
    payload.assign(b, pos, size_t(len));
    if (masked) for (size_t i = 0; i < payload.size(); ++i) payload[i] = char(uint8_t(payload[i]) ^ mask[i & 3]);
    return int(pos + len);
}

// ---- HTTP -------------------------------------------------------------------------------------

std::string HttpRequest::queryParam(const std::string& key) const {
    size_t p = 0;
    while (p < query.size()) {
        size_t e = query.find('&', p);
        if (e == std::string::npos) e = query.size();
        const std::string kv = query.substr(p, e - p);
        const size_t eq = kv.find('=');
        if (kv.substr(0, eq) == key) return eq == std::string::npos ? "" : kv.substr(eq + 1);
        p = e + 1;
    }
    return "";
}

struct HttpServer::Client {
    int fd = -1;
    std::string in, out;
    bool ws = false, closing = false;
    time_t last = 0;
};

namespace {
const char* statusText(int s) {
    switch (s) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    default: return "OK";
    }
}
void setNonBlocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }
}

bool HttpServer::start(const std::string& bindAddr, int port, Handler handler) {
    if (running_) return false;
    handler_ = std::move(handler);
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) return false;
    int on = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(uint16_t(port));
    if (::inet_pton(AF_INET, bindAddr.c_str(), &a.sin_addr) != 1) a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0 || ::listen(listenFd_, 32) < 0) {
        ::close(listenFd_); listenFd_ = -1;
        return false;
    }
    socklen_t al = sizeof a;
    ::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&a), &al);
    port_ = ntohs(a.sin_port);
    setNonBlocking(listenFd_);
    int p[2];
    if (::pipe(p) != 0) { ::close(listenFd_); listenFd_ = -1; return false; }
    wakeRd_ = p[0]; wakeWr_ = p[1];
    setNonBlocking(wakeRd_); setNonBlocking(wakeWr_);
    running_ = true;
    thread_ = std::thread([this] { run(); });
    return true;
}

void HttpServer::stop() {
    if (!running_.exchange(false)) return;
    const char c = 1;
    if (::write(wakeWr_, &c, 1) < 0) {}
    if (thread_.joinable()) thread_.join();
    ::close(listenFd_); ::close(wakeRd_); ::close(wakeWr_);
    listenFd_ = wakeRd_ = wakeWr_ = -1;
}

void HttpServer::broadcast(const std::string& text) {
    if (!running_) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (pending_.size() > 64) pending_.erase(pending_.begin());
        pending_.push_back(wsEncodeText(text));
    }
    const char c = 1;
    if (::write(wakeWr_, &c, 1) < 0) {}
}

void HttpServer::run() {
    std::list<Client> clients;
    auto drop = [&](std::list<Client>::iterator it) {
        if (it->ws) --wsCount_;
        ::close(it->fd);
        return clients.erase(it);
    };
    while (running_) {
        std::vector<pollfd> fds;
        fds.push_back({listenFd_, POLLIN, 0});
        fds.push_back({wakeRd_, POLLIN, 0});
        for (auto& c : clients) fds.push_back({c.fd, short(POLLIN | (c.out.empty() ? 0 : POLLOUT)), 0});
        ::poll(fds.data(), nfds_t(fds.size()), 1000);
        if (!running_) break;

        if (fds[1].revents & POLLIN) {
            char buf[64];
            while (::read(wakeRd_, buf, sizeof buf) > 0) {}
            std::vector<std::string> frames;
            { std::lock_guard<std::mutex> lk(mu_); frames.swap(pending_); }
            for (auto& c : clients) if (c.ws) {
                for (auto& f : frames) c.out += f;
                if (c.out.size() > (1u << 20)) c.closing = true;
            }
        }
        if (fds[0].revents & POLLIN) {
            for (;;) {
                int fd = ::accept(listenFd_, nullptr, nullptr);
                if (fd < 0) break;
                if (clients.size() >= 64) { ::close(fd); continue; }
                setNonBlocking(fd);
                int one = 1;
                ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                Client c; c.fd = fd; c.last = time(nullptr);
                clients.push_back(std::move(c));
            }
        }
        // fds[2..] correspond to clients in order as of poll time (new clients appended after).
        size_t idx = 2;
        const size_t polled = fds.size() - 2;
        size_t n = 0;
        for (auto it = clients.begin(); it != clients.end() && n < polled; ++n, ++idx) {
            Client& c = *it;
            const short rev = fds[idx].revents;
            bool dead = (rev & (POLLERR | POLLHUP | POLLNVAL)) && !(rev & POLLIN);
            if (!dead && (rev & POLLIN)) {
                char buf[8192];
                for (;;) {
                    ssize_t r = ::recv(c.fd, buf, sizeof buf, 0);
                    if (r > 0) { c.in.append(buf, size_t(r)); c.last = time(nullptr); if (c.in.size() > (2u << 20)) { dead = true; break; } }
                    else if (r == 0) { dead = true; break; }
                    else break;
                }
            }
            if (!dead && c.ws) {
                for (;;) {
                    int op = 0; std::string payload;
                    const int used = wsDecode(c.in, op, payload);
                    if (used < 0) { dead = true; break; }
                    if (used == 0) break;
                    c.in.erase(0, size_t(used));
                    if (op == 0x8) { c.out += std::string("\x88\x00", 2); c.closing = true; break; }
                    if (op == 0x9) { std::string f = "\x8a"; f += char(payload.size() & 0x7f); c.out += f + payload.substr(0, 125); }
                }
            } else if (!dead && !c.closing) {
                const size_t hend = c.in.find("\r\n\r\n");
                if (hend == std::string::npos) { if (c.in.size() > 16384) dead = true; }
                else {
                    HttpRequest req;
                    const std::string head = c.in.substr(0, hend);
                    size_t le = head.find("\r\n");
                    const std::string line = head.substr(0, le);
                    const size_t s1 = line.find(' '), s2 = line.find(' ', s1 + 1);
                    if (s1 == std::string::npos || s2 == std::string::npos) dead = true;
                    else {
                        req.method = line.substr(0, s1);
                        std::string target = line.substr(s1 + 1, s2 - s1 - 1);
                        const size_t q = target.find('?');
                        req.path = target.substr(0, q);
                        if (q != std::string::npos) req.query = target.substr(q + 1);
                        size_t p = le == std::string::npos ? head.size() : le + 2;
                        while (p < head.size()) {
                            size_t e = head.find("\r\n", p);
                            if (e == std::string::npos) e = head.size();
                            const std::string h = head.substr(p, e - p);
                            const size_t colon = h.find(':');
                            if (colon != std::string::npos) {
                                std::string k = h.substr(0, colon), v = h.substr(colon + 1);
                                std::transform(k.begin(), k.end(), k.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
                                v.erase(0, v.find_first_not_of(" \t"));
                                req.headers[k] = v;
                            }
                            p = e + 2;
                        }
                        size_t clen = 0;
                        if (req.headers.count("content-length")) clen = size_t(std::strtoul(req.headers["content-length"].c_str(), nullptr, 10));
                        if (clen > (1u << 20)) {
                            c.out += "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                            c.closing = true;
                        } else if (c.in.size() >= hend + 4 + clen) {
                            req.body = c.in.substr(hend + 4, clen);
                            c.in.erase(0, hend + 4 + clen);
                            auto up = req.headers.find("upgrade");
                            if (req.path == "/ws" && up != req.headers.end() && req.headers.count("sec-websocket-key")) {
                                c.out += "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                                         wsAcceptKey(req.headers["sec-websocket-key"]) + "\r\n\r\n";
                                c.ws = true;
                                ++wsCount_;
                                if (onWsOpen) c.out += wsEncodeText(onWsOpen());
                            } else {
                                HttpResponse resp;
                                try { resp = handler_(req); } catch (...) { resp.status = 500; resp.body = "{\"error\":\"internal\"}"; }
                                std::string o = "HTTP/1.1 " + std::to_string(resp.status) + " " + statusText(resp.status) + "\r\nContent-Type: " +
                                                resp.contentType + "\r\nContent-Length: " + std::to_string(resp.body.size()) +
                                                "\r\nCache-Control: no-store\r\nConnection: close\r\n";
                                for (auto& h : resp.headers) o += h.first + ": " + h.second + "\r\n";
                                o += "\r\n";
                                if (req.method != "HEAD") o += resp.body;
                                c.out += o;
                                c.closing = true;
                            }
                        }
                    }
                }
            }
            if (!dead && !c.out.empty()) {
                ssize_t w = ::send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
                if (w > 0) c.out.erase(0, size_t(w));
                else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) dead = true;
            }
            if (!dead && c.closing && c.out.empty()) dead = true;
            if (!dead && !c.ws && time(nullptr) - c.last > 30) dead = true;
            if (dead) it = drop(it); else ++it;
        }
    }
    for (auto it = clients.begin(); it != clients.end();) it = drop(it);
}

} // namespace shunt::app
