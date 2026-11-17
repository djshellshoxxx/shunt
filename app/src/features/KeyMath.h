// FS-02 key math (docs/specs/FS-02-key-clash-alarm.md): pure functions, no I/O, no threads.
// Camelot wheel representation, parsing of musical / Camelot / Open Key spellings, pitch shift and the
// compatibility level between two keys. Reference: Mixed In Key Camelot wheel
// (https://mixedinkey.com/harmonic-mixing-guide/), Open Key = Camelot number - 7 (mod 12), "d" major,
// "m" minor (Traktor / Beatport convention). Original Shunt code.
#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace shunt::app::keyclash {

struct Camelot {
    int number = 8;          // 1..12
    bool minor = true;       // true = letter A (minor), false = letter B (major)
    bool operator==(const Camelot& o) const { return number == o.number && minor == o.minor; }
    bool operator!=(const Camelot& o) const { return !(*this == o); }
};

enum class Level : uint8_t { Perfect, Compatible, Energy, Clash, Unknown };
const char* levelName(Level l);                    // "perfect" "compatible" "energy" "clash" "unknown"

// Accepts "Am", "F#m", "Bb", "A minor", "C♯ major", "Gmaj", "8A", "08a", "12B", "1d", "6m" (Open Key).
// Surrounding blanks are ignored. Anything else (empty, garbage, 13A, double accidentals) is nullopt.
std::optional<Camelot> parseKey(const std::string& s);
std::string toString(Camelot c);                   // "8A"
std::string toMusical(Camelot c);                  // "Am", "C", "F#m", "Ab" (Mixed In Key spelling)
std::string toOpenKey(Camelot c);                  // "1m", "1d"

// Moves by `semitones` (any sign): +7 wheel steps per semitone, mod 12.
Camelot shift(Camelot c, int semitones);
// round(12*log2(multiplier)); 0 for non-finite or non-positive multipliers; clamped to +-24.
int semitonesForPitch(double multiplier);

Level level(Camelot a, Camelot b);
Level level(const std::optional<Camelot>& a, const std::optional<Camelot>& b);   // Unknown if either is empty

// The CDJ-3000 status key bytes (0x15c..0x15e) are NOT decoded: their encoding is unknown (task K0,
// BUILD-PLAN R1). decodeStatusKey always returns nullopt until a capture exists.
std::optional<Camelot> decodeStatusKey(const uint8_t bytes[3]);
// UNVERIFIED hypothesis, only used when features.keyclash.experimentalStatusKey is true: the three
// bytes hold NUL-terminated ASCII text such as "8A" that parseKey accepts.
std::optional<Camelot> experimentalDecodeStatusKey(const uint8_t bytes[3]);

} // namespace shunt::app::keyclash
