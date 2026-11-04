// Small single-thread HTTP/1.1 + WebSocket (RFC 6455, text frames) server (RS-07 section 8).
// Request bodies are limited per path prefix (BUILD-PLAN R12, FS-05): 64 MiB by default, 256 MiB
// for FS-05 capture uploads; a larger Content-Length is answered with 413 before the body is read.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace shunt::app {

struct HttpRequest {
    std::string method, path, query, body;
    std::map<std::string, std::string> headers;   // lower-case names
    std::string queryParam(const std::string& key) const;
};

struct HttpResponse {
    int status = 200;
    std::string contentType = "application/json";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

std::string sha1(const std::string& data);                 // 20 raw bytes
std::string base64(const std::string& data);
std::string wsAcceptKey(const std::string& clientKey);
std::string wsEncodeText(const std::string& payload);      // unmasked server frame
// Decodes one client frame from buf; returns bytes consumed, 0 if incomplete, -1 if invalid.
int wsDecode(const std::string& buf, int& opcode, std::string& payload);

class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;
    static constexpr size_t kDefaultMaxBody = size_t(64) << 20;        // rekordbox XML import
    static constexpr size_t kReplayUploadMaxBody = size_t(256) << 20;  // FS-05 POST /api/f/replay/upload
    HttpServer();
    ~HttpServer() { stop(); }
    // port 0 picks a free port. Returns false when the socket cannot be bound.
    bool start(const std::string& bindAddr, int port, Handler handler);
    void stop();
    int port() const { return port_; }
    void broadcast(const std::string& text);
    size_t wsClients() const { return wsCount_; }
    // Called on the server thread for each new WebSocket client; the returned text is sent first.
    std::function<std::string()> onWsOpen;
    // Body limits. Call before start(). The default applies to every path; a prefix entry overrides
    // it for paths that start with the prefix (longest prefix wins; same prefix replaces).
    void setMaxBody(size_t bytes);
    void setMaxBody(const std::string& pathPrefix, size_t bytes);
    size_t maxBodyFor(const std::string& path) const;

private:
    struct Client;
    void run();
    int listenFd_ = -1, wakeRd_ = -1, wakeWr_ = -1, port_ = 0;
    Handler handler_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<size_t> wsCount_{0};
    std::mutex mu_;
    std::vector<std::string> pending_;
    size_t maxBody_ = kDefaultMaxBody;
    std::vector<std::pair<std::string, size_t>> routeMaxBody_;
};

} // namespace shunt::app
