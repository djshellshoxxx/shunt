// Compatibility badges (ES-04 section 4; research 02 section 4).
#pragma once
#include "shunt/net/Events.h"
#include <string>
#include <vector>

namespace shunt::core {

enum class Badge : uint8_t { Full, TempoOnly, Unsupported, Unverified };

struct CompatEntry {
    std::string model;         // display name, e.g. "CDJ-3000"
    std::string family;        // "Players", "All-in-one", "Mixers", "Software"
    Badge badge;
    std::string reason;
};

const char* badgeName(Badge b);
const std::vector<CompatEntry>& compatibilityTable();
// Badge for a live device, from its announced name and inferred model.
Badge badgeFor(const std::string& deviceName, net::Model model);
const CompatEntry* findCompat(const std::string& deviceName);

} // namespace shunt::core
