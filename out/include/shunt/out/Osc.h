// OSC output (RS-07 section 3): codec, profiles, beat scheduling.
#pragma once
#include "shunt/clock/ClockEngine.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace shunt::out {

struct OscArg {
    char type = 'i';
    int32_t i = 0;
    float f = 0;
    std::string s;
    static OscArg Int(int32_t v) { OscArg a; a.type = 'i'; a.i = v; return a; }
    static OscArg Float(float v) { OscArg a; a.type = 'f'; a.f = v; return a; }
    static OscArg Str(std::string v) { OscArg a; a.type = 's'; a.s = std::move(v); return a; }
};

std::vector<uint8_t> oscMessage(const std::string& address, const std::vector<OscArg>& args);

struct OscMessage {
    std::string address;
    std::vector<OscArg> args;
};
// Decodes one OSC message (i, f, s, T, F arguments). Bundles and malformed data return false.
bool oscParse(const uint8_t* data, size_t len, OscMessage& out);

// Non-blocking UDP listener for remote control (RS-07 addendum).
class OscInput {
public:
    ~OscInput();
    bool open(uint16_t port, std::string& error);
    void close();
    bool isOpen() const { return fd_ >= 0; }
    // Reads every pending datagram and calls fn for each valid message.
    void poll(const std::function<void(const OscMessage&)>& fn);
private:
    int fd_ = -1;
};

enum class OscProfile : uint8_t { Generic, Resolume, GrandMA3, MagicQ, QLab };
const char* oscProfileName(OscProfile p);
OscProfile oscProfileFromName(const std::string& s);

struct OscSettings {
    bool enabled = false;
    std::string host = "127.0.0.1";
    uint16_t port = 9000;
    OscProfile profile = OscProfile::Generic;
    int qlabMasterCue = 1, qlabTrackCue = 2;
};

class OscOutput {
public:
    using Sink = std::function<void(const std::vector<uint8_t>&)>;

    void configure(const OscSettings& s);
    const OscSettings& settings() const { return s_; }
    // Replace the default UDP sink (tests).
    void setSink(Sink sink) { sink_ = std::move(sink); }

    void poll(int64_t nowNs, const clock::Timeline& tl);
    void onMasterChanged(int device);
    void onTrackLoaded(int deck, uint32_t rekordboxId, const std::string& title, const std::string& artist);
    void onOnAir(int deck, bool on);

    uint64_t sent() const { return sent_; }
    const std::string& error() const { return error_; }
    bool connected() const { return s_.enabled && error_.empty(); }

private:
    void emit(const std::string& address, const std::vector<OscArg>& args);
    void emitTempo(double bpm);
    void emitBeat(const clock::Timeline& tl, int64_t beat);
    bool openSocket();

    OscSettings s_;
    Sink sink_;
    int fd_ = -1;
    uint32_t ip_ = 0;
    clock::PublishPolicy policy_;
    int64_t nextBeat_ = 0;
    bool haveBeat_ = false, lastPlaying_ = false;
    uint32_t lastReset_ = 0;
    uint64_t sent_ = 0;
    std::string error_;
public:
    ~OscOutput();
};

} // namespace shunt::out
