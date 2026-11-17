// FS-02 key math. See KeyMath.h. Original Shunt code.
#include "KeyMath.h"
#include <cmath>
#include <cctype>

namespace shunt::app::keyclash {

namespace {

// Constant Camelot table, index = number - 1 (Mixed In Key spelling). The tests check it against an
// independent pitch-class computation.
const char* const kMajor[12] = {"B", "F#", "Db", "Ab", "Eb", "Bb", "F", "C", "G", "D", "A", "E"};
const char* const kMinor[12] = {"Abm", "Ebm", "Bbm", "Fm", "Cm", "Gm", "Dm", "Am", "Em", "Bm", "F#m", "C#m"};

int wrap12(long v) { return int(((v % 12) + 12) % 12); }

Camelot fromPitchClass(int tonicPc, bool minor) {
    const int majorPc = minor ? wrap12(tonicPc + 3) : tonicPc;   // relative major of a minor key is +3
    return Camelot{wrap12(7 + long(majorPc) * 7) + 1, minor};    // C major = 8B; a fifth up = +1
}

// Replaces the UTF-8 sharp / flat signs with '#' / 'b' and drops natural signs.
std::string foldAccidentals(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i + 2 < s.size() + 0 && static_cast<unsigned char>(s[i]) == 0xE2 && static_cast<unsigned char>(s[i + 1]) == 0x99) {
            const unsigned char c = static_cast<unsigned char>(s[i + 2]);
            if (c == 0xAF) { o += '#'; i += 2; continue; }
            if (c == 0xAD) { o += 'b'; i += 2; continue; }
            if (c == 0xAE) { i += 2; continue; }
        }
        o += s[i];
    }
    return o;
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

const char* levelName(Level l) {
    switch (l) {
    case Level::Perfect: return "perfect";
    case Level::Compatible: return "compatible";
    case Level::Energy: return "energy";
    case Level::Clash: return "clash";
    case Level::Unknown: break;
    }
    return "unknown";
}

std::optional<Camelot> parseKey(const std::string& raw) {
    if (raw.size() > 40) return std::nullopt;
    std::string s = foldAccidentals(raw);
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    s = s.substr(b, e - b);
    if (s.empty()) return std::nullopt;

    // Camelot "8A" / Open Key "1d": one or two digits and one letter.
    if (std::isdigit(static_cast<unsigned char>(s[0]))) {
        size_t i = 0;
        int n = 0;
        while (i < s.size() && i < 2 && std::isdigit(static_cast<unsigned char>(s[i]))) n = n * 10 + (s[i++] - '0');
        if (i != s.size() - 1 || n < 1 || n > 12) return std::nullopt;
        switch (std::tolower(static_cast<unsigned char>(s[i]))) {
        case 'a': return Camelot{n, true};
        case 'b': return Camelot{n, false};
        case 'd': return Camelot{(n + 6) % 12 + 1, false};   // Open Key n -> Camelot n + 7
        case 'm': return Camelot{(n + 6) % 12 + 1, true};
        default: return std::nullopt;
        }
    }

    // Musical: note letter, optional accidental, optional mode word.
    const char l = char(std::tolower(static_cast<unsigned char>(s[0])));
    int pc;
    switch (l) {
    case 'c': pc = 0; break;
    case 'd': pc = 2; break;
    case 'e': pc = 4; break;
    case 'f': pc = 5; break;
    case 'g': pc = 7; break;
    case 'a': pc = 9; break;
    case 'b': pc = 11; break;
    default: return std::nullopt;
    }
    size_t i = 1;
    if (i < s.size() && (s[i] == '#' || s[i] == 'b')) { pc += s[i] == '#' ? 1 : -1; ++i; }
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    const std::string mode = s.substr(i);
    bool minor;
    if (mode.empty()) minor = false;
    else if (mode == "m") minor = true;
    else if (mode == "M") return std::nullopt;                       // ambiguous (major in some tools): reject
    else {
        const std::string m = lower(mode);
        if (m == "maj" || m == "major") minor = false;
        else if (m == "min" || m == "minor") minor = true;
        else return std::nullopt;
    }
    return fromPitchClass(wrap12(pc), minor);
}

std::string toString(Camelot c) { return std::to_string(c.number) + (c.minor ? "A" : "B"); }

std::string toMusical(Camelot c) {
    const int i = wrap12(c.number - 1);
    return c.minor ? kMinor[i] : kMajor[i];
}

std::string toOpenKey(Camelot c) { return std::to_string(wrap12(c.number - 8) + 1) + (c.minor ? "m" : "d"); }

Camelot shift(Camelot c, int semitones) {
    return Camelot{wrap12(long(c.number - 1) + 7L * semitones) + 1, c.minor};
}

int semitonesForPitch(double m) {
    if (!std::isfinite(m) || m <= 0) return 0;
    const double s = std::round(12.0 * std::log2(m));
    return int(s > 24 ? 24 : (s < -24 ? -24 : s));
}

Level level(Camelot a, Camelot b) {
    if (a == b) return Level::Perfect;
    const int d = wrap12(b.number - a.number);
    const int dist = d > 6 ? 12 - d : d;
    if (a.minor == b.minor) {
        if (dist == 1) return Level::Compatible;
        if (dist == 2) return Level::Energy;
        return Level::Clash;
    }
    return dist == 0 ? Level::Compatible : Level::Clash;   // relative major / minor: same number
}

Level level(const std::optional<Camelot>& a, const std::optional<Camelot>& b) {
    return a && b ? level(*a, *b) : Level::Unknown;
}

std::optional<Camelot> decodeStatusKey(const uint8_t[3]) { return std::nullopt; }

std::optional<Camelot> experimentalDecodeStatusKey(const uint8_t bytes[3]) {
    std::string s;
    for (int i = 0; i < 3 && bytes[i] != 0; ++i) {
        if (bytes[i] < 0x20 || bytes[i] > 0x7e) return std::nullopt;
        s += char(bytes[i]);
    }
    return parseKey(s);
}

} // namespace shunt::app::keyclash
