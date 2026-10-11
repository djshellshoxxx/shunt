// OSC 1.0 encoding and decoding (messages and bundles with timetags) plus the
// Shunt profile mapping (RS-01). No sockets here; see UdpSender.h.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace shunt::out {

using OscArg = std::variant<int32_t, float, std::string>;

struct OscMessage {
    std::string address;
    std::vector<OscArg> args;
};

// NTP timetag from a wall-clock time in ns since the Unix epoch; 1 = "immediately".
uint64_t oscTimetagFromUnixNs(int64_t unixNs);

std::vector<uint8_t> encodeOsc(const OscMessage& m);
std::vector<uint8_t> encodeOscBundle(const std::vector<OscMessage>& ms, uint64_t timetag);
// Decodes a message or flattens a bundle; empty on malformed input.
std::vector<OscMessage> decodeOsc(const uint8_t* data, size_t len);

} // namespace shunt::out
