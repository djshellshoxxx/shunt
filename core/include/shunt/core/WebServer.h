// Local HTTP and WebSocket server (civetweb): status page, OBS overlay
// (/overlay), Box configuration page, REST control and 10 Hz state over
// WebSocket (/ws). Pages are embedded in the binary.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace shunt::core {

class Engine;

struct WebServerOptions {
    uint16_t port = 8080;
    std::string bindAddress = "0.0.0.0";
    bool allowConfig = true;        // POST endpoints (Box); desktop may expose read-only
    bool allowUpdate = false;       // Box only: firmware bundle upload
    std::string updateDir;          // where uploaded bundles are written
};

class WebServer {
public:
    explicit WebServer(Engine& engine);
    ~WebServer();
    bool start(const WebServerOptions& opt);
    void stop();
    bool isRunning() const;
    uint16_t port() const;
    std::string lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace shunt::core
