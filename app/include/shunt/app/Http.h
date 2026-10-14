// Small single-thread HTTP/1.1 + WebSocket (RFC 6455, text frames) server (RS-07 section 8).
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
    ~HttpServer() { stop(); }
    // port 0 picks a free port. Returns false when the socket cannot be bound.
    bool start(const std::string& bindAddr, int port, Handler handler);
    void stop();
    int port() const { return port_; }
    void broadcast(const std::string& text);
    size_t wsClients() const { return wsCount_; }
    // Called on the server thread for each new WebSocket client; the returned text is sent first.
    std::function<std::string()> onWsOpen;

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
};

} // namespace shunt::app
