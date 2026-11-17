#include "DbProtocol.h"
#include <cstring>

namespace shunt::meta::db {

namespace {
void put16(std::vector<uint8_t>& o, uint32_t v) { o.push_back(uint8_t(v >> 8)); o.push_back(uint8_t(v)); }
void put32(std::vector<uint8_t>& o, uint32_t v) { put16(o, v >> 16); put16(o, v & 0xffff); }
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

void appendUtf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) s += char(cp);
    else if (cp < 0x800) { s += char(0xc0 | (cp >> 6)); s += char(0x80 | (cp & 0x3f)); }
    else if (cp < 0x10000) { s += char(0xe0 | (cp >> 12)); s += char(0x80 | ((cp >> 6) & 0x3f)); s += char(0x80 | (cp & 0x3f)); }
    else { s += char(0xf0 | (cp >> 18)); s += char(0x80 | ((cp >> 12) & 0x3f)); s += char(0x80 | ((cp >> 6) & 0x3f)); s += char(0x80 | (cp & 0x3f)); }
}

uint8_t argTypeFor(uint8_t tag) {
    if (tag == kTagBlob) return BenchTunables::kArgTypeBlob;
    if (tag == kTagString) return BenchTunables::kArgTypeString;
    return BenchTunables::kArgTypeNumber;
}
} // namespace

std::string utf16beToUtf8(const uint8_t* d, size_t units) {
    std::string s;
    s.reserve(units);
    for (size_t i = 0; i < units; ++i) {
        uint32_t u = uint32_t(d[2 * i]) << 8 | d[2 * i + 1];
        if (u == 0) break;
        if (u >= 0xd800 && u < 0xdc00 && i + 1 < units) {
            const uint32_t lo = uint32_t(d[2 * i + 2]) << 8 | d[2 * i + 3];
            if (lo >= 0xdc00 && lo < 0xe000) { appendUtf8(s, 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00)); ++i; continue; }
            u = 0xfffd;
        } else if (u >= 0xd800 && u < 0xe000) {
            u = 0xfffd;
        }
        appendUtf8(s, u);
    }
    return s;
}

std::vector<uint8_t> utf8ToUtf16be(const std::string& s) {
    std::vector<uint8_t> o;
    o.reserve(s.size() * 2 + 2);
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const size_t n = s.size();
    for (size_t i = 0; i < n;) {
        uint32_t cp = p[i];
        size_t len = cp < 0x80 ? 1 : (cp >> 5) == 6 ? 2 : (cp >> 4) == 14 ? 3 : (cp >> 3) == 30 ? 4 : 0;
        if (len == 0 || i + len > n) { cp = 0xfffd; len = 1; }
        else if (len > 1) {
            cp &= 0x3fu >> (len - 1);
            for (size_t k = 1; k < len; ++k) {
                if ((p[i + k] & 0xc0) != 0x80) { cp = 0xfffd; len = k; break; }
                cp = cp << 6 | (p[i + k] & 0x3f);
            }
        }
        i += len;
        if (cp > 0x10ffff || (cp >= 0xd800 && cp < 0xe000)) cp = 0xfffd;
        if (cp >= 0x10000) { cp -= 0x10000; put16(o, 0xd800 + (cp >> 10)); put16(o, 0xdc00 + (cp & 0x3ff)); }
        else put16(o, cp);
    }
    put16(o, 0);
    return o;
}

void encodeField(const Field& f, std::vector<uint8_t>& out) {
    out.push_back(f.tag);
    switch (f.tag) {
    case kTagU8: out.push_back(uint8_t(f.number)); break;
    case kTagU16: put16(out, f.number); break;
    case kTagU32: put32(out, f.number); break;
    case kTagBlob: put32(out, uint32_t(f.blob.size())); out.insert(out.end(), f.blob.begin(), f.blob.end()); break;
    case kTagString: {
        const auto u = utf8ToUtf16be(f.text);
        put32(out, uint32_t(u.size() / 2));
        out.insert(out.end(), u.begin(), u.end());
        break;
    }
    default: break;
    }
}

std::vector<uint8_t> encodeMessage(const Message& m) {
    std::vector<uint8_t> out;
    encodeField(Field::u32(kMagic), out);
    encodeField(Field::u32(m.transaction), out);
    encodeField(Field::u16(m.type), out);
    const size_t argc = m.args.size() < kMaxArgs ? m.args.size() : kMaxArgs;
    encodeField(Field::u8(uint8_t(argc)), out);
    std::vector<uint8_t> types(kMaxArgs, 0);
    for (size_t i = 0; i < argc; ++i) types[i] = argTypeFor(m.args[i].tag);
    encodeField(Field::bytes(std::move(types)), out);
    for (size_t i = 0; i < argc; ++i) encodeField(m.args[i], out);
    return out;
}

bool BufferSource::read(uint8_t* out, size_t n) {
    if (n > size_ - pos_) return false;
    std::memcpy(out, data_ + pos_, n);
    pos_ += n;
    return true;
}

DecodeError decodeField(ByteSource& in, Field& f) {
    uint8_t tag = 0, b[4];
    if (!in.read(&tag, 1)) return DecodeError::Io;
    f = Field{};
    f.tag = tag;
    switch (tag) {
    case kTagU8:
        if (!in.read(b, 1)) return DecodeError::Io;
        f.number = b[0];
        return DecodeError::None;
    case kTagU16:
        if (!in.read(b, 2)) return DecodeError::Io;
        f.number = uint32_t(b[0]) << 8 | b[1];
        return DecodeError::None;
    case kTagU32:
        if (!in.read(b, 4)) return DecodeError::Io;
        f.number = get32(b);
        return DecodeError::None;
    case kTagBlob: {
        if (!in.read(b, 4)) return DecodeError::Io;
        const uint32_t len = get32(b);
        if (len > kMaxBlobBytes) return DecodeError::TooLarge;
        f.blob.resize(len);
        if (len && !in.read(f.blob.data(), len)) return DecodeError::Io;
        return DecodeError::None;
    }
    case kTagString: {
        if (!in.read(b, 4)) return DecodeError::Io;
        const uint32_t units = get32(b);
        if (units > kMaxStringUnits) return DecodeError::TooLarge;
        std::vector<uint8_t> raw(size_t(units) * 2);
        if (units && !in.read(raw.data(), raw.size())) return DecodeError::Io;
        f.text = utf16beToUtf8(raw.data(), units);
        return DecodeError::None;
    }
    default:
        return DecodeError::BadTag;
    }
}

DecodeError decodeMessage(ByteSource& in, Message& m) {
    m = Message{};
    Field f;
    DecodeError e;
    if ((e = decodeField(in, f)) != DecodeError::None) return e;
    if (f.tag != kTagU32 || f.number != kMagic) return DecodeError::BadMagic;
    if ((e = decodeField(in, f)) != DecodeError::None) return e;
    if (f.tag != kTagU32) return DecodeError::BadHeader;
    m.transaction = f.number;
    if ((e = decodeField(in, f)) != DecodeError::None) return e;
    if (f.tag != kTagU16) return DecodeError::BadHeader;
    m.type = uint16_t(f.number);
    if ((e = decodeField(in, f)) != DecodeError::None) return e;
    if (f.tag != kTagU8 || f.number > kMaxArgs) return DecodeError::BadHeader;
    const size_t argc = f.number;
    if ((e = decodeField(in, f)) != DecodeError::None) return e;
    if (f.tag != kTagBlob || f.blob.size() != kMaxArgs) return DecodeError::BadHeader;
    m.args.resize(argc);
    for (size_t i = 0; i < argc; ++i)
        if ((e = decodeField(in, m.args[i])) != DecodeError::None) return e;
    return DecodeError::None;
}

} // namespace shunt::meta::db
