#include "shunt/out/RawMidiSink.h"
#include "shunt/net/Socket.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

namespace shunt::out {

RawMidiSink::~RawMidiSink() { close(); }

bool RawMidiSink::open(const std::string& path) {
    close();
    fd_ = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd_ < 0) { error_ = std::strerror(errno); return false; }
    error_.clear();
    return true;
}

void RawMidiSink::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

void RawMidiSink::send(int64_t atNs, const uint8_t* bytes, size_t len) {
    if (fd_ < 0) return;
    // Bounded wait: the generator's horizon is 20 ms, never sleep longer than 50 ms.
    int64_t wait = atNs - net::monotonicNowNs();
    if (wait > 50'000'000) wait = 50'000'000;
    if (wait > 0) {
        timespec ts{time_t(wait / 1'000'000'000), long(wait % 1'000'000'000)};
        nanosleep(&ts, nullptr);
    }
    if (::write(fd_, bytes, len) != ssize_t(len)) error_ = std::strerror(errno);
}

} // namespace shunt::out
