// ShuntSim (RS-05): emits a scripted 4-deck plus mixer player network on real UDP.
// Announce every 1.5 s, status every 200 ms, beats at the effective tempo.
#include "shunt/net/Packets.h"
#include "shunt/net/Socket.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <random>
#include <string>
#include <time.h>

using namespace shunt::net;

namespace {

constexpr int64_t MS = 1'000'000;

struct Deck {
    uint8_t number;
    const char* name;
    std::array<uint8_t, 6> mac;
    uint32_t ip;
    double baseBpm;
    double pitch = 1.0;
    bool playing = true;
    bool master = false;
    uint8_t handoffTo = 0xff;
    int64_t nextBeatNs = 0;
    int beatInBar = 1;
    uint32_t beatNumber = 1;
    bool loopOn = false;
    double bpm() const { return baseBpm * pitch; }
    double periodNs() const { return 60e9 / bpm(); }
};

struct Options {
    std::string dest = "127.0.0.1";
    std::string scenario = "handoff";
    double durationS = 20;
    double jitterMs = 0;
    double lossPct = 0;
    double outlierPct = 0;
    double outlierMs = 100;
    unsigned seed = 1;
    bool quiet = false;
};

void usage() {
    std::printf("shuntsim --dest IP [--scenario steady|handoff|nudge|pitch|hotcue|loop|pause|loss|jitter]\n"
                "         [--duration S] [--jitter-ms X] [--loss PCT] [--outlier-pct PCT] [--seed N] [--quiet]\n");
}

void sleepUntil(int64_t ns) {
    const int64_t now = monotonicNowNs();
    if (ns <= now) return;
    timespec ts{};
    ts.tv_sec = (ns - now) / 1'000'000'000LL;
    ts.tv_nsec = (ns - now) % 1'000'000'000LL;
    nanosleep(&ts, nullptr);
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--dest") o.dest = next();
        else if (a == "--scenario") o.scenario = next();
        else if (a == "--duration") o.durationS = std::atof(next().c_str());
        else if (a == "--jitter-ms") o.jitterMs = std::atof(next().c_str());
        else if (a == "--loss") o.lossPct = std::atof(next().c_str());
        else if (a == "--outlier-pct") o.outlierPct = std::atof(next().c_str());
        else if (a == "--seed") o.seed = unsigned(std::atoi(next().c_str()));
        else if (a == "--quiet") o.quiet = true;
        else { usage(); return a == "--help" ? 0 : 2; }
    }
    if (o.scenario == "loss" && o.lossPct == 0) o.lossPct = 3;
    if (o.scenario == "jitter") { if (o.jitterMs == 0) o.jitterMs = 5; if (o.outlierPct == 0) o.outlierPct = 1; }

    const uint32_t dest = parseIp(o.dest);
    if (dest == 0) { std::fprintf(stderr, "bad --dest\n"); return 2; }
    auto factory = makePosixSocketFactory();
    auto sock = factory->create();
    if (!sock->bind(0)) { std::fprintf(stderr, "cannot create socket\n"); return 1; }

    std::mt19937 rng(o.seed);
    std::normal_distribution<double> gauss(0.0, 1.0);
    std::uniform_real_distribution<double> uni(0.0, 1.0);

    const int64_t t0 = monotonicNowNs();
    Deck decks[4] = {
        {1, "CDJ-3000", {0, 0xe0, 0x36, 0xaa, 0, 1}, 0xc0a80101, 128.0},
        {2, "CDJ-3000", {0, 0xe0, 0x36, 0xaa, 0, 2}, 0xc0a80102, 130.0},
        {3, "CDJ-2000NXS2", {0, 0xe0, 0x36, 0xaa, 0, 3}, 0xc0a80103, 124.0},
        {4, "XDJ-1000MK2", {0, 0xe0, 0x36, 0xaa, 0, 4}, 0xc0a80104, 140.0},
    };
    decks[0].master = true;
    decks[2].playing = false;
    decks[3].playing = false;
    for (auto& d : decks) d.nextBeatNs = t0 + 200 * MS + int64_t(uni(rng) * d.periodNs());
    const std::array<uint8_t, 6> mixerMac{0, 0xe0, 0x36, 0xbb, 0, 0x21};

    uint8_t buf[1024];
    auto sendTo = [&](uint16_t port, size_t len) { sock->send(buf, len, dest, port); };

    int64_t nextKeepAlive = t0, nextStatus = t0 + 50 * MS;
    int64_t mixerNextBeat = t0 + 200 * MS;
    const int64_t endNs = t0 + int64_t(o.durationS * 1e9);
    int beatsSent = 0, beatsDropped = 0;
    bool stage1Done = false, stage2Done = false, stage3Done = false;
    int nudgeBeats = 0, rampBeats = 0;

    while (true) {
        const int64_t now = monotonicNowNs();
        if (now >= endNs) break;
        const double t = double(now - t0) / 1e9;

        // Scenario script
        if (o.scenario == "handoff") {
            if (t >= 6.0 && !stage1Done) { decks[0].handoffTo = 2; stage1Done = true; }
            if (t >= 6.4 && !stage2Done) { decks[0].master = false; decks[0].handoffTo = 0xff; decks[1].master = true; stage2Done = true; }
        } else if (o.scenario == "nudge") {
            if (t >= 6.0 && !stage1Done) { decks[0].nextBeatNs += 15 * MS; nudgeBeats = 4; stage1Done = true; }
        } else if (o.scenario == "pitch") {
            if (t >= 5.0 && !stage1Done) { rampBeats = 8; stage1Done = true; }
        } else if (o.scenario == "hotcue") {
            if (t >= 6.0 && !stage1Done) {
                decks[0].nextBeatNs = now + int64_t(decks[0].periodNs() * 0.37);
                decks[0].beatNumber = 200; decks[0].beatInBar = 1; stage1Done = true;
            }
        } else if (o.scenario == "loop") {
            if (t >= 6.0 && !stage1Done) { decks[0].loopOn = true; stage1Done = true; }
            if (t >= 12.0 && !stage2Done) { decks[0].loopOn = false; stage2Done = true; }
        } else if (o.scenario == "pause") {
            if (t >= 6.0 && !stage1Done) { decks[0].playing = false; stage1Done = true; }
            if (t >= 11.0 && !stage2Done) {
                decks[0].playing = true; decks[0].nextBeatNs = now + 123 * MS; decks[0].beatInBar = 3; stage2Done = true;
            }
        }
        if (t >= 2.0 && !stage3Done) { stage3Done = true; }

        int64_t nextEvent = endNs;
        // Keep-alives
        if (now >= nextKeepAlive) {
            for (auto& d : decks) {
                KeepAliveParams k; k.name = d.name; k.number = d.number; k.mac = d.mac; k.ip = d.ip; k.peerCount = 5;
                sendTo(kAnnouncePort, buildKeepAlive(buf, k));
            }
            KeepAliveParams m; m.name = "DJM-900NXS2"; m.kind = DeviceKind::Mixer; m.number = kMixerDeviceNumber; m.mac = mixerMac; m.ip = 0xc0a80121;
            sendTo(kAnnouncePort, buildKeepAlive(buf, m));
            nextKeepAlive = now + 1500 * MS;
        }
        nextEvent = std::min(nextEvent, nextKeepAlive);
        // Status
        if (now >= nextStatus) {
            for (auto& d : decks) {
                CdjStatusParams s; s.name = d.name; s.number = d.number; s.length = std::strcmp(d.name, "CDJ-3000") == 0 ? 0x200 : 0x11c;
                s.slot = 3; s.trackType = 1; s.rekordboxId = 1000u + d.number; s.trackNumber = d.number;
                s.playState = d.playing ? 3 : 5;
                s.flags = uint8_t((d.playing ? kFlagPlay : 0) | (d.master ? kFlagMaster : 0) | (d.playing ? kFlagOnAir : 0));
                s.pitch1 = d.pitch; s.bpm100 = uint16_t(std::lround(d.baseBpm * 100)); s.masterMeaning = d.master ? 2 : 0;
                s.handoffTo = d.handoffTo; s.beatNumber = d.beatNumber; s.beatInBar = uint8_t(d.beatInBar);
                s.nx = s.length == 0x200 ? 0x1f : 0x0f;
                sendTo(kStatusPort, buildCdjStatus(buf, s));
            }
            MixerStatusParams m;
            const Deck* master = nullptr;
            for (auto& d : decks) if (d.master) master = &d;
            m.flags = 0xd0; m.bpm100 = master ? uint16_t(std::lround(master->bpm() * 100)) : 0xffff;
            sendTo(kStatusPort, buildMixerStatus(buf, m));
            const uint8_t onAir[6] = {decks[0].playing, decks[1].playing, decks[2].playing, decks[3].playing, 0, 0};
            sendTo(kBeatPort, buildOnAir(buf, onAir, false));
            nextStatus = now + 200 * MS;
        }
        nextEvent = std::min(nextEvent, nextStatus);
        // Beats
        for (auto& d : decks) {
            if (!d.playing) continue;
            if (now >= d.nextBeatNs) {
                BeatParams b; b.name = d.name; b.number = d.number; b.pitch = d.pitch; b.bpm100 = uint16_t(std::lround(d.baseBpm * 100));
                b.beatInBar = uint8_t(d.beatInBar);
                const uint32_t p = uint32_t(d.periodNs() / MS);
                b.nextBeatMs = p; b.secondBeatMs = 2 * p; b.nextBarMs = uint32_t((5 - d.beatInBar) * p);
                b.fourthBeatMs = 4 * p; b.secondBarMs = b.nextBarMs + 4 * p; b.eighthBeatMs = 8 * p;
                const size_t len = buildBeat(buf, b);
                double delayMs = gauss(rng) * o.jitterMs;
                if (o.outlierPct > 0 && uni(rng) * 100 < o.outlierPct) delayMs += o.outlierMs;
                if (delayMs > 0) sleepUntil(now + int64_t(delayMs * MS));
                if (o.lossPct > 0 && uni(rng) * 100 < o.lossPct) ++beatsDropped;
                else { sendTo(kBeatPort, len); ++beatsSent; }
                if (&d == &decks[0] && rampBeats > 0) { d.pitch += 0.0025; --rampBeats; }
                if (&d == &decks[0] && nudgeBeats > 0 && --nudgeBeats == 0) d.nextBeatNs -= 15 * MS;
                d.nextBeatNs += int64_t(d.periodNs());
                if (d.loopOn) {
                    d.beatInBar = d.beatInBar % 4 + 1;
                    d.beatNumber = 9 + uint32_t(d.beatInBar - 1);
                } else {
                    d.beatInBar = d.beatInBar % 4 + 1;
                    ++d.beatNumber;
                }
            }
            nextEvent = std::min(nextEvent, d.nextBeatNs);
        }
        // Mixer beats follow the master's tempo
        if (now >= mixerNextBeat) {
            const Deck* master = nullptr;
            for (auto& d : decks) if (d.master) master = &d;
            BeatParams b; b.name = "DJM-900NXS2"; b.number = kMixerDeviceNumber;
            b.bpm100 = master ? uint16_t(std::lround(master->bpm() * 100)) : 0xffff; b.pitch = 1.0; b.beatInBar = 1;
            sendTo(kBeatPort, buildBeat(buf, b));
            mixerNextBeat = master ? master->nextBeatNs : now + 500 * MS;
            if (mixerNextBeat <= now) mixerNextBeat = now + 10 * MS;
        }
        nextEvent = std::min(nextEvent, mixerNextBeat);
        sleepUntil(std::min(nextEvent, now + 50 * MS));
    }
    if (!o.quiet) std::printf("shuntsim: scenario %s, %d beats sent, %d dropped\n", o.scenario.c_str(), beatsSent, beatsDropped);
    return 0;
}
