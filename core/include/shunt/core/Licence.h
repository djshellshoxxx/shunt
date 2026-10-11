// Licence key files (RS-06): Ed25519-signed JSON, verified offline.
#pragma once
#include <cstdint>
#include <string>

namespace shunt::core {

enum class Edition : uint8_t { Monitor, Full, Box, Venue };

struct Licence {
    Edition edition = Edition::Monitor;
    std::string licensee, email, keyId, issued;
    int machines = 3;
    bool valid = false;
    std::string error;
};

const char* editionName(Edition e);
// Key file format: {"payload": "<base64 JSON>", "sig": "<base64 64-byte Ed25519>"}.
Licence verifyLicence(const std::string& keyFileText);
Licence loadLicenceFile(const std::string& path);
// Vendor side (shunt_keygen): sign a payload with a 32-byte secret seed (hex).
std::string signLicence(const std::string& payloadJson, const std::string& secretSeedHex);
std::string publicKeyFromSeed(const std::string& secretSeedHex);   // hex
// Outputs enabled for this edition (Monitor: none). Builds with
// SHUNT_REQUIRE_LICENCE=OFF always return true.
bool outputsAllowed(const Licence& l);

} // namespace shunt::core
