// POSIX raw MIDI sink: writes bytes to a character device or FIFO (ALSA /dev/snd/midiC*D*,
// a UART for DIN MIDI, or a pipe). Sleeps until each message's timestamp (ES-05 section 2).
#pragma once
#include "shunt/out/MidiClock.h"
#include <string>

namespace shunt::out {

class RawMidiSink : public IMidiSink {
public:
    RawMidiSink() = default;
    ~RawMidiSink() override;
    RawMidiSink(const RawMidiSink&) = delete;
    RawMidiSink& operator=(const RawMidiSink&) = delete;

    bool open(const std::string& path);
    void close();
    bool isOpen() const { return fd_ >= 0; }
    const std::string& lastError() const { return error_; }
    void send(int64_t atNs, const uint8_t* bytes, size_t len) override;

private:
    int fd_ = -1;
    std::string error_;
};

} // namespace shunt::out
