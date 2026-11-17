// shunt_headless: engine + outputs + web UI on one port (Box service and desktop engine).
#include "shunt/app/Api.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

using namespace shunt::app;

namespace {
std::atomic<bool> g_stop{false};
void onSignal(int) { g_stop = true; }
}

int main(int argc, char** argv) {
    std::string configPath = Settings::defaultPath(), dataDir, iface, addr;
    int port = -1;
    bool passive = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--config") configPath = next();
        else if (a == "--data") dataDir = next();
        else if (a == "--port") port = std::atoi(next().c_str());
        else if (a == "--iface") iface = next();
        else if (a == "--addr") addr = next();
        else if (a == "--passive") passive = true;
        else {
            std::printf("shunt_headless [--config FILE] [--data DIR] [--port N] [--iface NAME | --addr IP] [--passive]\n"
                        "Serves the web UI and API on http://0.0.0.0:<port> (default 8080).\n");
            return a == "--help" ? 0 : 2;
        }
    }
    std::string err;
    Settings s = Settings::load(configPath, &err);
    if (!err.empty()) std::fprintf(stderr, "warning: %s (using defaults)\n", err.c_str());
    if (!iface.empty()) s.interfaceName = iface;
    if (!addr.empty()) s.interfaceName = addr;
    if (passive) s.mode = "passive";
    if (port >= 0) s.httpPort = port;
    if (dataDir.empty()) {
        const char* h = std::getenv("HOME");
        dataDir = h && *h ? std::string(h) + "/.local/share/shunt" : "shunt-data";
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGPIPE, SIG_IGN);

    Core core(s, configPath, dataDir);
    HttpServer server;
    if (!server.start(s.httpBind, s.httpPort, [&](const HttpRequest& r) { return handleRequest(core, r); })) {
        std::fprintf(stderr, "cannot listen on %s:%d\n", s.httpBind.c_str(), s.httpPort);
        return 1;
    }
    server.onWsOpen = [&] { return core.status().dump(); };
    std::string startErr;
    core.start(&startErr);
    if (!startErr.empty()) std::fprintf(stderr, "network: %s\n", startErr.c_str());
    std::printf("Shunt web UI: http://localhost:%d/\n", server.port());
    std::fflush(stdout);

    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (server.wsClients() > 0) server.broadcast(core.status().dump());
    }
    server.stop();
    core.stop();
    return 0;
}
