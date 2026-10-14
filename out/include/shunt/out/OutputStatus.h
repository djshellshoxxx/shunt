// Shared status block for every output (ES-05 section 5).
#pragma once
#include <cstdint>
#include "shunt/clock/ClockEngine.h"
#include <string>

namespace shunt::out {

struct OutputStatus {
    bool enabled = false;
    bool connected = false;
    std::string lastError;
    uint64_t messagesSent = 0;
    double jitterNsRms = 0;
};

// Outputs ignore timelines outside a sane musical range (also bounds message rates).
inline bool usable(const clock::Timeline& tl) { return tl.bpm >= 20.0 && tl.bpm <= 400.0; }

} // namespace shunt::out
