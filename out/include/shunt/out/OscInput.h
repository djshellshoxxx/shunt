// OSC input (RS-03): decodes /shunt/baroffset i and /shunt/beatonly i into control actions.
#pragma once
#include "shunt/out/Osc.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace shunt::out {

// Decodes one OSC message (not a bundle). Returns nullopt on malformed input.
std::optional<OscMessage> decodeOsc(const uint8_t* data, size_t len);
// Decodes a message or a bundle (nested bundles flattened, time tags ignored).
std::vector<OscMessage> decodeOscPacket(const uint8_t* data, size_t len);

struct OscControl {
    std::function<void(int barOffset)> setBarOffset;   // 0..3
    std::function<void(bool beatOnly)> setBeatOnly;
};

// Dispatches control messages; returns the number handled. Out-of-range values are ignored.
size_t dispatchOscControl(const std::vector<OscMessage>& msgs, const OscControl& ctl);

} // namespace shunt::out
