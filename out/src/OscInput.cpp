#include "shunt/out/OscInput.h"
#include <cstring>

namespace shunt::out {

namespace {

// Reads a NUL-terminated, 4-byte padded string at pos; advances pos.
bool readString(const uint8_t* d, size_t len, size_t& pos, std::string& out) {
    size_t e = pos;
    while (e < len && d[e]) ++e;
    if (e >= len) return false;
    out.assign(reinterpret_cast<const char*>(d + pos), e - pos);
    pos = (e + 4) & ~size_t(3);
    return pos <= len;
}

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

void decodeInto(const uint8_t* d, size_t len, int depth, std::vector<OscMessage>& out) {
    if (len >= 8 && std::memcmp(d, "#bundle", 8) == 0) {
        if (depth > 4) return;
        size_t pos = 16;
        while (pos + 4 <= len) {
            const size_t n = be32(d + pos);
            pos += 4;
            if (n > len - pos) return;
            decodeInto(d + pos, n, depth + 1, out);
            pos += n;
        }
    } else if (auto m = decodeOsc(d, len)) {
        out.push_back(std::move(*m));
    }
}

} // namespace

std::optional<OscMessage> decodeOsc(const uint8_t* d, size_t len) {
    if (!d || len < 4 || len % 4 || d[0] != '/') return std::nullopt;
    OscMessage m;
    size_t pos = 0;
    std::string tags;
    if (!readString(d, len, pos, m.address)) return std::nullopt;
    if (pos >= len) return m;                       // address only (legacy, no type tags)
    if (!readString(d, len, pos, tags) || tags.empty() || tags[0] != ',') return std::nullopt;
    for (size_t i = 1; i < tags.size(); ++i) {
        switch (tags[i]) {
        case 'i':
            if (pos + 4 > len) return std::nullopt;
            m.args.emplace_back(int32_t(be32(d + pos))); pos += 4; break;
        case 'f': {
            if (pos + 4 > len) return std::nullopt;
            const uint32_t u = be32(d + pos); float f; std::memcpy(&f, &u, 4);
            m.args.emplace_back(f); pos += 4; break;
        }
        case 's': {
            std::string s;
            if (!readString(d, len, pos, s)) return std::nullopt;
            m.args.emplace_back(std::move(s)); break;
        }
        default: return std::nullopt;               // unsupported type: reject the message
        }
    }
    return m;
}

std::vector<OscMessage> decodeOscPacket(const uint8_t* d, size_t len) {
    std::vector<OscMessage> out;
    if (d) decodeInto(d, len, 0, out);
    return out;
}

size_t dispatchOscControl(const std::vector<OscMessage>& msgs, const OscControl& ctl) {
    size_t handled = 0;
    for (auto& m : msgs) {
        if (m.args.size() != 1 || !std::holds_alternative<int32_t>(m.args[0])) continue;
        const int32_t v = std::get<int32_t>(m.args[0]);
        if (m.address == "/shunt/baroffset" && ctl.setBarOffset && v >= 0 && v <= 3) { ctl.setBarOffset(v); ++handled; }
        else if (m.address == "/shunt/beatonly" && ctl.setBeatOnly && (v == 0 || v == 1)) { ctl.setBeatOnly(v == 1); ++handled; }
    }
    return handled;
}

} // namespace shunt::out
