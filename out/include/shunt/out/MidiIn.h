// MIDI input (FS-04 section "Input ports"): a pure byte-stream parser, an input-port interface,
// a poll(2) based fd port for ALSA raw devices / FIFOs / socketpairs, and a small reader thread.
// No ALSA library: like the clock output (Midi.cpp) it reads the raw device node.
// Platform independent except FdMidiInPort/openRawMidiIn (POSIX). Original Shunt code.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace shunt::out {

struct MidiMessage {
    enum Kind : uint8_t { NoteOff, NoteOn, PolyPressure, ControlChange, ProgramChange, ChannelPressure, PitchBend };
    Kind kind = NoteOff;
    uint8_t channel = 0;   // 0..15
    uint8_t number = 0;    // note / controller / program / pressure value / pitch LSB (0..127)
    uint8_t value = 0;     // velocity / controller value / pitch MSB (0..127); 0 where there is no second byte
};

// Byte-at-a-time parser. Handles running status, real-time bytes (0xF8..0xFF) interleaved
// anywhere (they never disturb the message being assembled), SysEx (skipped up to 0xF7 or the
// next status byte), system common messages (skipped, they cancel running status), truncated
// messages (a status byte aborts the pending one) and stray data bytes (dropped).
// Note On with velocity 0 is reported as Note Off. Never throws, never allocates.
class MidiParser {
public:
    bool feed(uint8_t byte, MidiMessage& out);   // true when `out` holds a complete message
    void reset();
private:
    uint8_t running_ = 0;    // 0x80..0xEF or 0
    uint8_t data_[2] = {0, 0};
    uint8_t have_ = 0;
    bool sysex_ = false;
};

// Blocking-with-timeout byte source.
class IMidiInPort {
public:
    virtual ~IMidiInPort() = default;
    // Waits up to timeoutMs. Returns bytes read (>0), 0 on timeout, -1 when the port failed
    // (device unplugged, peer closed) and should be reopened.
    virtual int read(uint8_t* buf, size_t cap, int timeoutMs) = 0;
    virtual std::string name() const = 0;
};

// Owns an fd (closed in the destructor). Works for character devices (ALSA rawmidi), FIFOs
// (EOF without a writer is not an error) and sockets/pipes (EOF is an error).
class FdMidiInPort : public IMidiInPort {
public:
    FdMidiInPort(int fd, std::string name);
    ~FdMidiInPort() override;
    FdMidiInPort(const FdMidiInPort&) = delete;
    FdMidiInPort& operator=(const FdMidiInPort&) = delete;
    int read(uint8_t* buf, size_t cap, int timeoutMs) override;
    std::string name() const override { return name_; }
private:
    int fd_;
    bool fifo_ = false;
    std::string name_;
};

// Opens `path` read-only and non-blocking. Accepts character devices and FIFOs only (a regular
// file is refused). nullptr and `error` set on failure.
std::unique_ptr<IMidiInPort> openRawMidiIn(const std::string& path, std::string& error);

// Reader thread: opens the port with `opener` (retrying every ~1 s while it fails or after a
// read error), parses the bytes and calls `onMessage` on the reader thread. stop() joins and
// takes at most about one poll interval (20 ms).
class MidiInThread {
public:
    using Opener = std::function<std::unique_ptr<IMidiInPort>(std::string& error)>;
    using Handler = std::function<void(const MidiMessage&)>;
    MidiInThread(Opener opener, Handler onMessage);
    ~MidiInThread();
    MidiInThread(const MidiInThread&) = delete;
    MidiInThread& operator=(const MidiInThread&) = delete;
    void start();
    void stop();
    bool connected() const { return connected_.load(); }
    std::string lastError() const;
    uint64_t bytesRead() const { return bytes_.load(); }

private:
    void run();
    Opener opener_;
    Handler handler_;
    std::thread th_;
    std::atomic<bool> stop_{false}, connected_{false};
    std::atomic<uint64_t> bytes_{0};
    mutable std::mutex mu_;
    std::string error_;
};

} // namespace shunt::out
