#include "shunt/out/Midi.h"
#include <cmath>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>

namespace shunt::out {

double MidiClockGenerator::tickTimeNs(int64_t n, const clock::Timeline& tl) const {
    return double(tl.beatOriginNs) + (double(n) / 24.0 - double(tl.beatIndex)) * tl.periodNs();
}

int MidiClockGenerator::beatInBarOf(int64_t beat, const clock::Timeline& tl) const {
    if (!tl.barKnown) return 0;
    return int(((int64_t(tl.beatInBar - 1) + (beat - tl.beatIndex)) % 4 + 4) % 4) + 1;
}

void MidiClockGenerator::resync(int64_t nowNs, const clock::Timeline& tl) {
    const double pos = double(tl.beatIndex) + double(nowNs - tl.beatOriginNs) / tl.periodNs();
    nextTick_ = int64_t(std::ceil(pos * 24.0));
}

void MidiClockGenerator::poll(int64_t nowNs, int64_t lookaheadNs, const clock::Timeline& tl, std::vector<MidiMsg>& out) {
    auto push = [&](int64_t t, uint8_t a, uint8_t b = 0, uint8_t c = 0, uint8_t len = 1) {
        t = std::max(t, lastEmitNs_);
        MidiMsg m; m.timeNs = t; m.bytes[0] = a; m.bytes[1] = b; m.bytes[2] = c; m.len = len;
        lastEmitNs_ = t;
        out.push_back(m);
    };
    if (tl.bpm <= 0) return;

    if (!locked_) {
        locked_ = true;
        lastReset_ = tl.resetSequence;
        resync(nowNs, tl);
        pendingStart_ = opt_.startOnFirstLock;
    } else if (tl.resetSequence != lastReset_) {
        lastReset_ = tl.resetSequence;
        resync(nowNs, tl);
        if (!stopped_) {
            if (opt_.startOnReset) pendingStart_ = true;
            else if (opt_.sppOnReset) {
                const int64_t beat = nextTick_ / 24;
                const int inBar = std::max(1, beatInBarOf(beat, tl));
                const int sixteenths = (inBar - 1) * 4 + int((nextTick_ % 24) / 6);
                push(int64_t(tickTimeNs(nextTick_, tl)), 0xF2, uint8_t(sixteenths & 0x7f), uint8_t((sixteenths >> 7) & 0x7f), 3);
                push(int64_t(tickTimeNs(nextTick_, tl)), 0xFB);
            }
        }
    }

    if (opt_.pause == MidiClockOptions::Stop) {
        if (!tl.playing && !stopped_) { push(nowNs, 0xFC); stopped_ = true; pendingContinue_ = false; return; }
        if (tl.playing && stopped_) { stopped_ = false; pendingContinue_ = true; resync(nowNs, tl); }
    }
    if (stopped_) return;

    if (tickTimeNs(nextTick_, tl) < double(nowNs - 50'000'000)) resync(nowNs, tl);   // stalled: do not burst

    int guard = 0;
    while (guard++ < 64) {
        const double t = tickTimeNs(nextTick_, tl);
        if (t >= double(nowNs + lookaheadNs)) break;
        const bool beatBoundary = nextTick_ % 24 == 0;
        if (beatBoundary) {
            const int inBar = beatInBarOf(nextTick_ / 24, tl);
            if (pendingStart_ && (!tl.barKnown || inBar == 1)) { push(int64_t(t), 0xFA); pendingStart_ = false; }
            else if (pendingContinue_) { push(int64_t(t), 0xFB); pendingContinue_ = false; }
        }
        if (!pendingStart_ && !pendingContinue_) push(int64_t(t), 0xF8);
        ++nextTick_;
    }
}

namespace {
class FdMidiPort : public IMidiPort {
public:
    FdMidiPort(int fd, std::string n) : fd_(fd), name_(std::move(n)) {}
    ~FdMidiPort() override { if (fd_ >= 0) ::close(fd_); }
    bool write(const uint8_t* d, size_t n) override { return ::write(fd_, d, n) == ssize_t(n); }
    std::string name() const override { return name_; }
private:
    int fd_;
    std::string name_;
};
}

std::unique_ptr<IMidiPort> openRawMidiPort(const std::string& path, std::string& error) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_NONBLOCK);
    if (fd < 0) { error = "cannot open " + path + ": " + std::strerror(errno); return nullptr; }
    return std::make_unique<FdMidiPort>(fd, path);
}

std::vector<MidiPortInfo> listMidiPorts() {
    std::vector<MidiPortInfo> v;
    if (DIR* d = ::opendir("/dev/snd")) {
        while (dirent* e = ::readdir(d))
            if (std::strncmp(e->d_name, "midiC", 5) == 0) v.push_back({std::string("/dev/snd/") + e->d_name, e->d_name});
        ::closedir(d);
    }
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.id < b.id; });
    return v;
}

} // namespace shunt::out
