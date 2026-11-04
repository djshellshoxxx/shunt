// ShuntSim engine (see Sim.h). Virtual-clock event loop; packets use the net/ builders.
// Original Shunt code.
#include "Sim.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <time.h>

using namespace shunt::net;

namespace shuntsim {

std::vector<ScenarioInfo>& scenarioRegistry() {
    static std::vector<ScenarioInfo> r;
    return r;
}

Sim::Sim(const Options& o) : opt(o), rng_(o.seed) {}

bool Sim::init(std::string* err) {
    if (!opt.dest.empty()) {
        destIp_ = parseIp(opt.dest);
        if (destIp_ == 0) { *err = "bad --dest"; return false; }
        factory_ = makePosixSocketFactory();
        sock_ = factory_->create();
        if (!sock_->bind(0)) { *err = "cannot create socket"; return false; }
    } else {
        destIp_ = 0xc0a801ff;
    }
    if (opt.dest.empty() && opt.record.empty()) { *err = "need --dest or --record"; return false; }
    if (!opt.record.empty() && !writer_.open(opt.record)) { *err = "cannot open " + opt.record; return false; }

    const ScenarioInfo* info = nullptr;
    for (auto& s : scenarioRegistry()) if (opt.scenario == s.name) info = &s;
    if (!info) {
        *err = "unknown scenario '" + opt.scenario + "'; available:";
        for (auto& s : scenarioRegistry()) *err += std::string(" ") + s.name;
        return false;
    }
    scenarioName = info->name;

    std::uniform_real_distribution<double> uni(0.0, 1.0);
    Deck init[4] = {
        {1, "CDJ-3000", {0, 0xe0, 0x36, 0xaa, 0, 1}, 0xc0a80101, 128.0},
        {2, "CDJ-3000", {0, 0xe0, 0x36, 0xaa, 0, 2}, 0xc0a80102, 130.0},
        {3, "CDJ-2000NXS2", {0, 0xe0, 0x36, 0xaa, 0, 3}, 0xc0a80103, 124.0},
        {4, "XDJ-1000MK2", {0, 0xe0, 0x36, 0xaa, 0, 4}, 0xc0a80104, 140.0},
    };
    for (int i = 0; i < 4; ++i) decks[i] = init[i];
    decks[0].master = true;
    decks[2].playing = false;
    decks[3].playing = false;
    for (auto& d : decks) d.nextBeatNs = 200 * MS + int64_t(uni(rng_) * d.periodNs());

    info->setup(*this);
    if (opt.durationS >= 0) durationS = opt.durationS;
    std::stable_sort(actions_.begin(), actions_.end(), [](const Action& a, const Action& b) { return a.t < b.t; });
    wallStart_ = monotonicNowNs();
    return true;
}

void Sim::at(double tSeconds, std::function<void(Sim&)> fn) { actions_.push_back({tSeconds, std::move(fn)}); }

void Sim::log(const char* fmt, ...) {
    if (!opt.verbose) return;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    std::printf("scenario %s t=%.3f %s\n", scenarioName, t(), msg);
}

void Sim::setChannel(int n, bool on) {
    mixerChannel[n - 1] = on ? 1 : 0;
    decks[n - 1].onAirFlag = on ? 1 : 0;
    log("deck %d channel %s", n, on ? "on air" : "off air");
}

void Sim::loadTrack(int n, uint32_t id, const uint8_t key[3]) {
    Deck& d = decks[n - 1];
    d.trackId = id;
    d.hasKey = key != nullptr;
    if (key) std::memcpy(d.key, key, 3);
    log("deck %d loads track %u", n, id);
}

void Sim::setMaster(int n) {
    for (auto& d : decks) { d.master = d.number == n; d.handoffTo = 0xff; }
    log("deck %d master", n);
}

const Deck* Sim::masterDeck() const {
    for (auto& d : decks) if (d.master) return &d;
    return nullptr;
}

void Sim::sendNow(uint32_t srcIp, uint16_t port, const uint8_t* data, size_t len) {
    if (sock_) sock_->send(data, len, destIp_, port);
    if (writer_.isOpen()) {
        CapturedDatagram c;
        c.timestampNs = 1'700'000'000LL * 1'000'000'000LL + now_;
        c.srcIp = srcIp; c.dstIp = destIp_; c.srcPort = port; c.dstPort = port;
        c.payload.assign(data, data + len);
        writer_.write(c);
    }
}

void Sim::emit(uint32_t srcIp, uint16_t port, const uint8_t* data, size_t len, int64_t delayNs) {
    if (delayNs > 0) pending_.push_back({now_ + delayNs, seq_++, srcIp, port, std::vector<uint8_t>(data, data + len)});
    else sendNow(srcIp, port, data, len);
}

void Sim::waitUntil(int64_t virtualNs) {
    if (opt.timeScale <= 0) return;
    const int64_t target = wallStart_ + int64_t(double(virtualNs) / opt.timeScale);
    const int64_t wall = monotonicNowNs();
    if (target <= wall) return;
    timespec ts{};
    ts.tv_sec = (target - wall) / 1'000'000'000LL;
    ts.tv_nsec = (target - wall) % 1'000'000'000LL;
    nanosleep(&ts, nullptr);
}

void Sim::run() {
    const std::array<uint8_t, 6> mixerMac{0, 0xe0, 0x36, 0xbb, 0, 0x21};
    constexpr uint32_t mixerIp = 0xc0a80121;
    std::normal_distribution<double> gauss(0.0, 1.0);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    uint8_t buf[1024];
    int64_t nextKeepAlive = 0, nextStatus = 50 * MS, mixerNextBeat = 200 * MS;
    const int64_t endNs = int64_t(durationS * 1e9);

    while (now_ < endNs) {
        // Delayed (jittered) datagrams that are due.
        std::sort(pending_.begin(), pending_.end(), [](const Pending& a, const Pending& b) { return a.at != b.at ? a.at < b.at : a.seq < b.seq; });
        size_t done = 0;
        while (done < pending_.size() && pending_[done].at <= now_) {
            sendNow(pending_[done].srcIp, pending_[done].port, pending_[done].data.data(), pending_[done].data.size());
            ++done;
        }
        pending_.erase(pending_.begin(), pending_.begin() + long(done));

        // Scenario script
        while (nextAction_ < actions_.size() && int64_t(actions_[nextAction_].t * 1e9) <= now_) {
            auto fn = actions_[nextAction_].fn;
            ++nextAction_;
            fn(*this);
        }

        int64_t nextEvent = endNs;
        // Keep-alives
        if (now_ >= nextKeepAlive) {
            for (auto& d : decks) {
                KeepAliveParams k; k.name = d.name; k.number = d.number; k.mac = d.mac; k.ip = d.ip; k.peerCount = 5;
                emit(d.ip, kAnnouncePort, buf, buildKeepAlive(buf, k));
            }
            KeepAliveParams m; m.name = "DJM-900NXS2"; m.kind = DeviceKind::Mixer; m.number = kMixerDeviceNumber; m.mac = mixerMac; m.ip = mixerIp;
            emit(mixerIp, kAnnouncePort, buf, buildKeepAlive(buf, m));
            nextKeepAlive = now_ + 1500 * MS;
        }
        nextEvent = std::min(nextEvent, nextKeepAlive);
        // Status
        if (now_ >= nextStatus) {
            for (auto& d : decks) {
                CdjStatusParams s; s.name = d.name; s.number = d.number; s.length = std::strcmp(d.name, "CDJ-3000") == 0 ? 0x200 : 0x11c;
                s.slot = 3; s.trackType = 1; s.rekordboxId = d.trackId ? d.trackId : 1000u + d.number; s.trackNumber = d.number;
                s.playState = d.playing ? 3 : 5;
                const bool onAir = d.onAirFlag < 0 ? d.playing : d.onAirFlag != 0;
                s.flags = uint8_t((d.playing ? kFlagPlay : 0) | (d.master ? kFlagMaster : 0) | (onAir ? kFlagOnAir : 0));
                s.pitch1 = d.pitch; s.bpm100 = uint16_t(std::lround(d.baseBpm * 100)); s.masterMeaning = d.master ? 2 : 0;
                s.handoffTo = d.handoffTo; s.beatNumber = d.beatNumber; s.beatInBar = uint8_t(d.reportedBeatInBar());
                s.nx = s.length == 0x200 ? 0x1f : 0x0f;
                if (d.hasKey && s.length >= 0x15f) std::memcpy(s.key, d.key, 3);
                emit(d.ip, kStatusPort, buf, buildCdjStatus(buf, s));
            }
            MixerStatusParams m;
            const Deck* master = masterDeck();
            m.flags = 0xd0; m.bpm100 = master ? uint16_t(std::lround(master->bpm() * 100)) : 0xffff;
            emit(mixerIp, kStatusPort, buf, buildMixerStatus(buf, m));
            if (mixerOnAirPackets) {
                uint8_t onAir[6] = {0, 0, 0, 0, 0, 0};
                for (int i = 0; i < 4; ++i) onAir[i] = mixerChannel[i] < 0 ? decks[i].playing : mixerChannel[i] != 0;
                emit(mixerIp, kBeatPort, buf, buildOnAir(buf, onAir, false));
            }
            nextStatus = now_ + 200 * MS;
        }
        nextEvent = std::min(nextEvent, nextStatus);
        // Beats
        for (auto& d : decks) {
            if (!d.playing) continue;
            if (now_ >= d.nextBeatNs) {
                BeatParams b; b.name = d.name; b.number = d.number; b.pitch = d.pitch; b.bpm100 = uint16_t(std::lround(d.baseBpm * 100));
                b.beatInBar = uint8_t(d.reportedBeatInBar());
                const uint32_t p = uint32_t(d.periodNs() / MS);
                b.nextBeatMs = p; b.secondBeatMs = 2 * p; b.nextBarMs = uint32_t((5 - d.beatInBar) * p);
                b.fourthBeatMs = 4 * p; b.secondBarMs = b.nextBarMs + 4 * p; b.eighthBeatMs = 8 * p;
                const size_t len = buildBeat(buf, b);
                double delayMs = gauss(rng_) * opt.jitterMs;
                if (opt.outlierPct > 0 && uni(rng_) * 100 < opt.outlierPct) delayMs += opt.outlierMs;
                if (opt.lossPct > 0 && uni(rng_) * 100 < opt.lossPct) ++beatsDropped_;
                else { emit(d.ip, kBeatPort, buf, len, delayMs > 0 ? int64_t(delayMs * MS) : 0); ++beatsSent_; }
                if (&d == &decks[0] && rampBeats > 0) { d.pitch += 0.0025; --rampBeats; }
                if (&d == &decks[0] && nudgeBeats > 0 && --nudgeBeats == 0) d.nextBeatNs -= 15 * MS;
                d.nextBeatNs += int64_t(d.periodNs());
                d.beatInBar = d.beatInBar % 4 + 1;
                if (d.loopOn) {
                    d.beatNumber = 9 + uint32_t(d.beatInBar - 1);
                } else if (d.loopLen > 0) {
                    d.beatNumber = d.loopStart + (d.beatNumber + 1 - d.loopStart) % d.loopLen;
                } else {
                    ++d.beatNumber;
                }
                if (d.derivedBar) d.beatInBar = int((d.beatNumber - 1) % 4) + 1;
            }
            nextEvent = std::min(nextEvent, d.nextBeatNs);
        }
        // Mixer beats follow the master's tempo
        if (now_ >= mixerNextBeat) {
            const Deck* master = masterDeck();
            BeatParams b; b.name = "DJM-900NXS2"; b.number = kMixerDeviceNumber;
            b.bpm100 = master ? uint16_t(std::lround(master->bpm() * 100)) : 0xffff; b.pitch = 1.0; b.beatInBar = 1;
            emit(mixerIp, kBeatPort, buf, buildBeat(buf, b));
            mixerNextBeat = master ? master->nextBeatNs : now_ + 500 * MS;
            if (mixerNextBeat <= now_) mixerNextBeat = now_ + 10 * MS;
        }
        nextEvent = std::min(nextEvent, mixerNextBeat);
        for (auto& p : pending_) nextEvent = std::min(nextEvent, p.at);
        if (nextAction_ < actions_.size()) nextEvent = std::min(nextEvent, int64_t(actions_[nextAction_].t * 1e9));
        nextEvent = std::max(nextEvent, now_ + 1);
        if (nextEvent >= endNs) break;
        waitUntil(nextEvent);
        now_ = nextEvent;
    }
    waitUntil(endNs);
    writer_.close();
}

void Sim::summary() const {
    if (!opt.quiet) std::printf("shuntsim: scenario %s, %d beats sent, %d dropped\n", scenarioName, beatsSent_, beatsDropped_);
}

} // namespace shuntsim
