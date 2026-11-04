// FS-05 Replay a capture: pure parts (docs/specs/FS-05-replay-a-capture.md). pcapng validation
// for uploads and files, the compact in-memory capture, file-name rules, and ReplayPlayer, the
// synthetic-time transport (pause, seek, speed, loop) that feeds due datagrams to a sink. No
// threads, no sockets, no clocks: time is always passed in. The IFeature wiring is in Replay.cpp.
// Original Shunt code.
#pragma once
#include "shunt/net/Pcapng.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace shunt::app::replay {

constexpr uint64_t kMaxUploadBytes = uint64_t(256) << 20;   // BUILD-PLAN R12
constexpr uint64_t kMaxFileBytes = uint64_t(500) << 20;     // files already in the captures folder
constexpr size_t kMaxDatagrams = 5'000'000;
constexpr size_t kMaxBlocks = 20'000'000;
constexpr size_t kMaxNameLen = 128;
constexpr int64_t kSeekWarmupNs = 30'000'000'000LL;          // history fed before a seek target
constexpr size_t kStepBudget = 5000;                         // datagrams per step (bounds the Core lock hold)

// ---- pcapng structure check -----------------------------------------------------------------
// Little-endian pcapng only (what PcapngWriter and Wireshark on x86/ARM write). Checks the file
// starts with a Section Header Block with the byte-order magic, every block length is >= 12, a
// multiple of 4, inside the file and repeated at the block end, and that Enhanced Packet Blocks
// hold their captured length. Sets `error` to a one-line reason on failure.
struct PcapngCheck {
    bool ok = false;
    std::string error;
    uint64_t blocks = 0, packets = 0;
};
PcapngCheck checkPcapng(const uint8_t* data, size_t len, uint64_t maxBytes);
PcapngCheck checkPcapngFile(const std::string& path, uint64_t maxBytes);

// ---- capture -------------------------------------------------------------------------------
struct Datagram {
    int64_t relNs;          // since the first kept datagram
    uint64_t off;           // into Capture::arena
    uint16_t len, port;
    uint32_t srcIp;
};
struct SeenDevice { int number; std::string name; };
struct Capture {
    std::vector<Datagram> dgrams;     // ports 50000..50002 only, in capture order, relNs non-decreasing
    std::vector<uint8_t> arena;
    int64_t firstTsNs = 0;            // capture (wall) clock of the first kept datagram
    int64_t durationNs = 0;
    uint64_t ignored = 0;             // datagrams on other ports
    std::vector<SeenDevice> devices;  // from keep-alives, sorted by number
};
// Builds the compact capture. False (and error) when there is no datagram on 50000..50002 or the
// datagram cap is exceeded. Timestamps going backwards are clamped to the previous one.
bool buildCapture(const std::vector<net::CapturedDatagram>& in, Capture& out, std::string& error);
// checkPcapngFile + PcapngReader + buildCapture.
bool loadCapture(const std::string& path, Capture& out, std::string& error, uint64_t maxBytes = kMaxFileBytes);

// ---- names ---------------------------------------------------------------------------------
// A bare file name: [A-Za-z0-9._-], 1..128 chars, ends ".pcapng", does not start with '.'.
bool validName(const std::string& name);
// Upload names: every other byte becomes '_'; leading dots are dropped; ".pcapng" required.
// Returns "" when nothing valid is left.
std::string sanitizeName(const std::string& name);

// ---- transport -----------------------------------------------------------------------------
struct ReplaySink {
    virtual ~ReplaySink() = default;
    virtual void feed(uint16_t port, const uint8_t* data, size_t len, int64_t recvNs, uint32_t srcIp, int64_t wallMs) = 0;
    virtual void reset(bool clearSession) = 0;   // new engine and device table (and session)
};

// Allowed speeds (FS-05): 0.25 0.5 1 2 4 8. Any number snaps to the nearest one (in log scale).
double snapSpeed(double s);

// Time mapping: the replay position p (capture ns since the first datagram) advances as
// anchorPos + (now - anchorNow) * speed while playing. Datagram d is fed once p >= d.relNs with
// recvNs = engineOffset + d.relNs, i.e. the engine always sees capture-time spacing (so it locks
// at any speed) and no jumps between reset points; engineOffset is chosen at each reset point
// (start, seek, loop wrap) so that at speed 1 recvNs tracks the monotonic clock.
class ReplayPlayer {
public:
    void load(std::shared_ptr<const Capture> cap, double speed, bool loop, int64_t nowNs);
    void unload() { cap_.reset(); }
    bool loaded() const { return cap_ != nullptr; }
    const Capture* capture() const { return cap_.get(); }

    void pause(int64_t nowNs);
    void resume(int64_t nowNs);
    void seek(int64_t posNs, int64_t nowNs);          // clamped to [0, duration]; resets on next step
    void setSpeed(double speed, int64_t nowNs);
    void setLoop(bool on) { loop_ = on; }

    // Feeds due datagrams (at most `budget`) and handles reset points and the end of the file.
    size_t step(int64_t nowNs, ReplaySink& sink, size_t budget = kStepBudget);

    int64_t positionNs(int64_t nowNs) const;
    int64_t durationNs() const { return cap_ ? cap_->durationNs : 0; }
    double speed() const { return speed_; }
    bool paused() const { return paused_; }
    bool loop() const { return loop_; }
    bool ended() const { return ended_; }
    uint64_t loops() const { return loops_; }
    uint64_t resets() const { return resets_; }
    uint64_t fed() const { return fed_; }
    size_t nextIndex() const { return idx_; }
    bool resetPending() const { return pendingReset_; }

private:
    void doReset(int64_t targetNs, int64_t nowNs, ReplaySink& sink);
    std::shared_ptr<const Capture> cap_;
    size_t idx_ = 0;
    int64_t anchorNow_ = 0, anchorPos_ = 0, engineOffset_ = 0;
    double speed_ = 1;
    bool paused_ = false, loop_ = false, ended_ = false, pendingReset_ = false;
    uint64_t loops_ = 0, resets_ = 0, fed_ = 0;
};

} // namespace shunt::app::replay
