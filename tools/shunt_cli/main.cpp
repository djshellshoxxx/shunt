// Minimal console: joins the player network (Follow or Passive) or replays a
// pcapng file, and prints devices, master, filtered BPM and beat/bar lines.
#include "shunt/net/NetworkStack.h"
#include "shunt/clock/ClockEngine.h"
#include "shunt/log/Tracklist.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

using namespace shunt;

namespace {

const char* stateName(clock::State s) {
    switch (s) {
    case clock::State::Idle: return "Idle";
    case clock::State::Locking: return "Locking";
    case clock::State::Locked: return "Locked";
    case clock::State::Coasting: return "Coasting";
    case clock::State::Paused: return "Paused";
    case clock::State::MixerMaster: return "MixerMaster";
    }
    return "?";
}

struct Stats {
    size_t devicesJoined = 0, masterChanges = 0, beats = 0;
    uint8_t lastMasterFrom = 0, lastMasterTo = 0;
    double lastBpm = 0;
};

void handleEvent(const net::Event& ev, clock::ClockEngine& engine, net::NetworkStack& stack, Stats& st, bool verbose) {
    engine.onEvent(ev);
    const auto& tl = engine.timeline();
    switch (ev.type) {
    case net::EventType::Device: {
        const net::Device* d = stack.devices().find(ev.device);
        const char* kind = ev.deviceEvent.kind == net::DeviceEvent::Joined ? "joined" :
                           ev.deviceEvent.kind == net::DeviceEvent::Left ? "left" : "model";
        std::printf("device %u %s %s (%s)\n", ev.device, kind, d ? d->name.c_str() : "", net::modelName(ev.deviceEvent.model));
        if (ev.deviceEvent.kind == net::DeviceEvent::Joined) ++st.devicesJoined;
        break;
    }
    case net::EventType::MasterChanged:
        std::printf("master %u -> %u%s\n", ev.masterChanged.from, ev.masterChanged.to, ev.masterChanged.inferred ? " (inferred)" : "");
        ++st.masterChanges;
        st.lastMasterFrom = ev.masterChanged.from;
        st.lastMasterTo = ev.masterChanged.to;
        break;
    case net::EventType::Beat:
        if (ev.beat.isMaster) {
            ++st.beats;
            st.lastBpm = tl.bpm;
            if (verbose)
                std::printf("beat %lld bar %d%s bpm %.2f conf %.2f %s reset %u\n", (long long)tl.beatIndex, tl.beatInBar,
                            tl.barKnown ? "" : "?", tl.bpm, double(tl.confidence), stateName(engine.state()), tl.resetSequence);
        }
        break;
    default:
        break;
    }
}

int replay(const std::string& path, double speed, bool verbose) {
    auto captured = net::PcapngReader::readAll(path);
    if (captured.empty()) { std::fprintf(stderr, "no UDP datagrams in %s\n", path.c_str()); return 1; }
    auto factory = net::makePosixSocketFactory();
    net::StackConfig cfg;
    cfg.mode = net::Mode::Follow;
    cfg.filterSubnet = false;
    net::NetworkStack stack(*factory, cfg);     // not started: no sockets, parsers only
    clock::ClockEngine engine;
    Stats st;
    const int64_t base = net::monotonicNowNs();
    const int64_t ts0 = captured.front().timestampNs;
    for (const auto& d : captured) {
        const int64_t recvNs = base + int64_t(double(d.timestampNs - ts0) / speed);
        if (speed > 0) {
            const int64_t now = net::monotonicNowNs();
            if (recvNs > now) { timespec ts{(recvNs - now) / 1'000'000'000LL, (recvNs - now) % 1'000'000'000LL}; nanosleep(&ts, nullptr); }
        }
        stack.feed(d.dstPort, d.payload.data(), d.payload.size(), recvNs, d.srcIp);
        stack.runTimers(recvNs);
        engine.tick(recvNs);
        net::Event ev;
        while (stack.pop(ev)) handleEvent(ev, engine, stack, st, verbose);
    }
    std::printf("replayed %zu datagrams: %llu parsed, %llu parse errors, %zu devices, %zu master changes, %zu master beats, bpm %.2f\n",
                captured.size(), (unsigned long long)stack.counters().parsed, (unsigned long long)stack.counters().parseErrors,
                st.devicesJoined, st.masterChanges, st.beats, st.lastBpm);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string addr, iface, replayPath, simPath, capturePath;
    net::Mode mode = net::Mode::Follow;
    uint8_t number = 1;
    double durationS = 0, speed = 1.0;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--addr") addr = next();
        else if (a == "--iface") iface = next();
        else if (a == "--mode") { std::string m = next(); mode = m == "passive" ? net::Mode::Passive : m == "lead" ? net::Mode::Lead : net::Mode::Follow; }
        else if (a == "--number") number = uint8_t(std::atoi(next().c_str()));
        else if (a == "--replay") replayPath = next();
        else if (a == "--speed") speed = std::atof(next().c_str());
        else if (a == "--duration") durationS = std::atof(next().c_str());
        else if (a == "--record") capturePath = next();
        else if (a == "--selftest-sim") simPath = next();
        else if (a == "--verbose" || a == "-v") verbose = true;
        else {
            std::printf("shunt_cli [--addr IP | --iface NAME] [--mode follow|passive|lead] [--number N] [--duration S]\n"
                        "          [--record out.pcapng] [--verbose] | --replay file.pcapng [--speed X]\n");
            return a == "--help" ? 0 : 2;
        }
    }
    if (!replayPath.empty()) return replay(replayPath, speed, verbose);

    // Interface selection: by name, by address, else the first non-loopback interface.
    auto ifaces = net::enumerateInterfaces();
    net::InterfaceInfo chosen;
    bool found = false;
    for (auto& i : ifaces) {
        if ((!iface.empty() && i.name == iface) || (!addr.empty() && i.address == net::parseIp(addr))) { chosen = i; found = true; break; }
    }
    if (!found && iface.empty() && addr.empty())
        for (auto& i : ifaces) if ((i.address >> 24) != 127) { chosen = i; found = true; break; }
    if (!found) { std::fprintf(stderr, "no matching interface\n"); return 1; }
    const bool loopback = (chosen.address >> 24) == 127;

    net::StackConfig cfg;
    cfg.mode = mode;
    cfg.address = chosen.address;
    cfg.netmask = chosen.netmask;
    cfg.broadcast = loopback ? chosen.address : chosen.broadcast;
    cfg.mac = chosen.mac;
    if (cfg.mac == std::array<uint8_t, 6>{}) cfg.mac = {0x02, 0x53, 0x48, 0x55, 0x4e, 0x54};
    cfg.leadNumber = number;
    cfg.capturePath = capturePath;

    auto factory = net::makePosixSocketFactory();
    net::NetworkStack stack(*factory, cfg);
    if (!stack.start()) { std::fprintf(stderr, "cannot bind sockets (ports 50000-50002 in use?)\n"); return 1; }
    std::printf("shunt_cli on %s (%s) mode %s, timestamps: %s\n", chosen.name.c_str(), net::ipToString(chosen.address).c_str(),
                mode == net::Mode::Passive ? "passive" : mode == net::Mode::Lead ? "lead" : "follow", stack.timestampSource());

    pid_t simPid = 0;
    if (!simPath.empty()) {
        std::string destStr = net::ipToString(chosen.address);
        const char* args[] = {simPath.c_str(), "--dest", destStr.c_str(), "--scenario", "handoff", "--duration", "12", "--jitter-ms", "0.5", nullptr};
        if (posix_spawn(&simPid, simPath.c_str(), nullptr, nullptr, const_cast<char**>(args), environ) != 0) {
            std::fprintf(stderr, "cannot spawn %s\n", simPath.c_str());
            return 1;
        }
        durationS = 13;
    }

    clock::ClockEngine engine;
    Stats st;
    const int64_t start = net::monotonicNowNs();
    int64_t nextReport = start + 1'000'000'000LL;
    uint8_t lastClaim = 0;
    while (durationS <= 0 || net::monotonicNowNs() - start < int64_t(durationS * 1e9)) {
        stack.poll(50);
        const int64_t now = net::monotonicNowNs();
        engine.tick(now);
        net::Event ev;
        while (stack.pop(ev)) handleEvent(ev, engine, stack, st, verbose);
        if (stack.claim().state() == net::ClaimMachine::State::Active && stack.claim().number() != lastClaim) {
            lastClaim = stack.claim().number();
            std::printf("claimed device number %u\n", lastClaim);
        }
        if (now >= nextReport) {
            nextReport += 1'000'000'000LL;
            const auto& tl = engine.timeline();
            std::printf("devices %zu master %u state %s bpm %.2f beat %lld bar %d conf %.2f\n", stack.devices().size(), engine.master(),
                        stateName(engine.state()), tl.bpm, (long long)tl.beatIndex, tl.beatInBar, double(tl.confidence));
        }
    }
    stack.stop();
    if (simPid > 0) {
        int status = 0;
        waitpid(simPid, &status, 0);
        const bool ok = st.devicesJoined >= 5 && st.masterChanges >= 2 && st.lastMasterFrom == 1 && st.lastMasterTo == 2 &&
                        st.beats > 20 && st.lastBpm > 129.9 && st.lastBpm < 130.1;
        std::printf("selftest: devices %zu, master changes %zu (last %u -> %u), master beats %zu, bpm %.2f: %s\n",
                    st.devicesJoined, st.masterChanges, st.lastMasterFrom, st.lastMasterTo, st.beats, st.lastBpm, ok ? "OK" : "FAIL");
        return ok ? 0 : 1;
    }
    return 0;
}
