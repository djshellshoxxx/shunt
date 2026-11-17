// MIDI input parser, fd port and reader thread (FS-04). Original Shunt code.
#include "shunt/out/MidiIn.h"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

namespace shunt::out {

// ---- parser ------------------------------------------------------------------------------------

void MidiParser::reset() {
    running_ = 0;
    have_ = 0;
    sysex_ = false;
}

bool MidiParser::feed(uint8_t b, MidiMessage& out) {
    if (b >= 0xF8) return false;                 // real-time: no effect on the message in progress
    if (b >= 0xF0) {
        have_ = 0;
        running_ = 0;                            // system messages cancel running status
        sysex_ = (b == 0xF0);                    // 0xF7 and the other system common bytes end/skip
        return false;
    }
    if (b >= 0x80) {
        sysex_ = false;
        running_ = b;
        have_ = 0;                               // a pending truncated message is dropped
        return false;
    }
    if (sysex_ || running_ == 0) return false;   // SysEx payload or data without a status byte
    const uint8_t need = ((running_ & 0xF0) == 0xC0 || (running_ & 0xF0) == 0xD0) ? 1 : 2;
    data_[have_++] = b;
    if (have_ < need) return false;
    have_ = 0;                                   // running status stays for the next message
    out.channel = running_ & 0x0F;
    out.number = data_[0];
    out.value = need == 2 ? data_[1] : 0;
    switch (running_ & 0xF0) {
        case 0x80: out.kind = MidiMessage::NoteOff; break;
        case 0x90: out.kind = out.value == 0 ? MidiMessage::NoteOff : MidiMessage::NoteOn; break;
        case 0xA0: out.kind = MidiMessage::PolyPressure; break;
        case 0xB0: out.kind = MidiMessage::ControlChange; break;
        case 0xC0: out.kind = MidiMessage::ProgramChange; break;
        case 0xD0: out.kind = MidiMessage::ChannelPressure; break;
        default: out.kind = MidiMessage::PitchBend; break;
    }
    return true;
}

// ---- fd port -----------------------------------------------------------------------------------

FdMidiInPort::FdMidiInPort(int fd, std::string name) : fd_(fd), name_(std::move(name)) {
    struct stat st;
    if (fd_ >= 0 && ::fstat(fd_, &st) == 0) fifo_ = S_ISFIFO(st.st_mode);
}

FdMidiInPort::~FdMidiInPort() {
    if (fd_ >= 0) ::close(fd_);
}

int FdMidiInPort::read(uint8_t* buf, size_t cap, int timeoutMs) {
    pollfd p{fd_, POLLIN, 0};
    const int r = ::poll(&p, 1, timeoutMs);
    if (r < 0) return errno == EINTR ? 0 : -1;
    if (r == 0) return 0;
    if (p.revents & (POLLERR | POLLNVAL)) return -1;
    if (p.revents & POLLIN) {
        const ssize_t n = ::read(fd_, buf, cap);
        if (n > 0) return int(n);
        if (n < 0) return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
        // EOF
    }
    if (fifo_) {                                  // no writer at the moment: idle, do not spin
        ::usleep(useconds_t(timeoutMs > 0 ? timeoutMs : 1) * 1000);
        return 0;
    }
    return -1;
}

std::unique_ptr<IMidiInPort> openRawMidiIn(const std::string& path, std::string& error) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { error = path + ": " + std::strerror(errno); return nullptr; }
    struct stat st;
    if (::fstat(fd, &st) != 0 || !(S_ISCHR(st.st_mode) || S_ISFIFO(st.st_mode))) {
        ::close(fd);
        error = path + ": not a MIDI device";
        return nullptr;
    }
    return std::make_unique<FdMidiInPort>(fd, path);
}

// ---- reader thread -----------------------------------------------------------------------------

MidiInThread::MidiInThread(Opener opener, Handler onMessage) : opener_(std::move(opener)), handler_(std::move(onMessage)) {}

MidiInThread::~MidiInThread() { stop(); }

void MidiInThread::start() {
    if (th_.joinable()) return;
    stop_ = false;
    th_ = std::thread([this] { run(); });
}

void MidiInThread::stop() {
    stop_ = true;
    if (th_.joinable()) th_.join();
    connected_ = false;
}

std::string MidiInThread::lastError() const {
    std::lock_guard<std::mutex> lk(mu_);
    return error_;
}

void MidiInThread::run() {
    using clock = std::chrono::steady_clock;
    std::unique_ptr<IMidiInPort> port;
    MidiParser parser;
    clock::time_point nextOpen = clock::now();
    uint8_t buf[256];
    while (!stop_.load()) {
        if (!port) {
            if (clock::now() < nextOpen) { ::usleep(20000); continue; }
            std::string err;
            try { port = opener_(err); } catch (...) { port.reset(); err = "open failed"; }
            if (!port) {
                { std::lock_guard<std::mutex> lk(mu_); error_ = err.empty() ? "cannot open port" : err; }
                nextOpen = clock::now() + std::chrono::seconds(1);
                continue;
            }
            parser.reset();
            { std::lock_guard<std::mutex> lk(mu_); error_.clear(); }
            connected_ = true;
        }
        const int n = port->read(buf, sizeof buf, 20);
        if (n < 0) {
            port.reset();
            connected_ = false;
            { std::lock_guard<std::mutex> lk(mu_); error_ = "input lost, retrying"; }
            nextOpen = clock::now() + std::chrono::seconds(1);
            continue;
        }
        bytes_ += uint64_t(n);
        for (int i = 0; i < n; ++i) {
            MidiMessage m;
            if (!parser.feed(buf[i], m)) continue;
            try { handler_(m); } catch (...) { /* a handler error must not kill the reader */ }
        }
    }
    connected_ = false;
}

} // namespace shunt::out
