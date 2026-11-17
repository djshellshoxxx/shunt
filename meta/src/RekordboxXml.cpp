// rekordbox XML export reader (RS-04 metadata path 2). A small streaming tag
// scanner: only start/end tag names and the attributes of COLLECTION/TRACK matter.
#include "shunt/meta/Metadata.h"
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace shunt::meta {

namespace {

constexpr size_t kChunk = 256 * 1024;
constexpr size_t kMaxMarkup = 4u << 20;   // a single tag/comment larger than this is treated as corrupt

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

void appendUtf8(std::string& s, uint32_t cp) {
    if (cp == 0 || cp > 0x10ffff || (cp >= 0xd800 && cp < 0xe000)) cp = 0xfffd;
    if (cp < 0x80) s += char(cp);
    else if (cp < 0x800) { s += char(0xc0 | (cp >> 6)); s += char(0x80 | (cp & 0x3f)); }
    else if (cp < 0x10000) { s += char(0xe0 | (cp >> 12)); s += char(0x80 | ((cp >> 6) & 0x3f)); s += char(0x80 | (cp & 0x3f)); }
    else { s += char(0xf0 | (cp >> 18)); s += char(0x80 | ((cp >> 12) & 0x3f)); s += char(0x80 | ((cp >> 6) & 0x3f)); s += char(0x80 | (cp & 0x3f)); }
}

// Decodes the five predefined entities and numeric references; anything else stays literal.
std::string decodeEntities(const char* p, size_t n) {
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n;) {
        if (p[i] != '&') { out += p[i++]; continue; }
        size_t end = i + 1;
        while (end < n && end - i <= 12 && p[end] != ';') ++end;
        if (end >= n || p[end] != ';') { out += p[i++]; continue; }
        const char* e = p + i + 1;
        const size_t len = end - i - 1;
        auto is = [&](const char* name) { return std::strlen(name) == len && std::memcmp(e, name, len) == 0; };
        bool done = true;
        if (is("amp")) out += '&';
        else if (is("lt")) out += '<';
        else if (is("gt")) out += '>';
        else if (is("quot")) out += '"';
        else if (is("apos")) out += '\'';
        else if (len >= 2 && e[0] == '#') {
            const bool hex = e[1] == 'x' || e[1] == 'X';
            uint32_t cp = 0;
            size_t k = hex ? 2 : 1;
            if (k >= len) done = false;
            for (; done && k < len; ++k) {
                const char c = e[k];
                uint32_t d;
                if (c >= '0' && c <= '9') d = uint32_t(c - '0');
                else if (hex && c >= 'a' && c <= 'f') d = uint32_t(c - 'a' + 10);
                else if (hex && c >= 'A' && c <= 'F') d = uint32_t(c - 'A' + 10);
                else { done = false; break; }
                cp = cp * (hex ? 16 : 10) + d;
                if (cp > 0x10ffff) cp = 0x110000;
            }
            if (done) appendUtf8(out, cp);
        } else done = false;
        if (!done) { out += p[i++]; continue; }
        i = end + 1;
    }
    return out;
}

// Locale-independent decimal parser; accepts '.' or ',' as the separator.
bool parseDecimal(const std::string& s, double& out) {
    size_t i = 0;
    while (i < s.size() && isSpace(s[i])) ++i;
    bool neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
    double v = 0, scale = 1;
    bool digits = false, frac = false;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c >= '0' && c <= '9') {
            digits = true;
            if (frac) { scale /= 10; v += (c - '0') * scale; }
            else v = v * 10 + (c - '0');
        } else if ((c == '.' || c == ',') && !frac) frac = true;
        else break;
    }
    if (!digits) return false;
    out = neg ? -v : v;
    return true;
}

bool parseU32(const std::string& s, uint32_t& out) {
    size_t i = 0;
    while (i < s.size() && isSpace(s[i])) ++i;
    uint64_t v = 0;
    size_t start = i;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
        v = v * 10 + uint64_t(s[i] - '0');
        if (v > 0xffffffffull) return false;
    }
    if (i == start) return false;
    while (i < s.size() && isSpace(s[i])) ++i;
    if (i != s.size()) return false;
    out = uint32_t(v);
    return true;
}

// Calls fn(name, nameLen, value) for each attribute in the raw tag body.
template <typename Fn>
void forEachAttribute(const char* p, size_t n, Fn&& fn) {
    size_t i = 0;
    while (i < n) {
        while (i < n && (isSpace(p[i]) || p[i] == '/')) ++i;
        const size_t ns = i;
        while (i < n && !isSpace(p[i]) && p[i] != '=' && p[i] != '/') ++i;
        const size_t nl = i - ns;
        while (i < n && isSpace(p[i])) ++i;
        if (i >= n || p[i] != '=') { if (nl == 0 && i < n) ++i; continue; }   // valueless attribute
        ++i;
        while (i < n && isSpace(p[i])) ++i;
        if (i >= n) break;
        size_t vs, ve;
        if (p[i] == '"' || p[i] == '\'') {
            const char q = p[i++];
            vs = i;
            while (i < n && p[i] != q) ++i;
            ve = i;
            if (i < n) ++i;
        } else {
            vs = i;
            while (i < n && !isSpace(p[i])) ++i;
            ve = i;
        }
        if (nl) fn(p + ns, nl, decodeEntities(p + vs, ve - vs));
    }
}

class Loader {
public:
    explicit Loader(std::map<uint32_t, TrackMetadata>& out) : out_(out) {}

    // Consumes as much of buf as forms complete markup; returns bytes consumed.
    size_t feed(const char* buf, size_t n) {
        size_t i = 0;
        while (i < n) {
            const void* lt = std::memchr(buf + i, '<', n - i);
            if (!lt) return n;
            i = size_t(static_cast<const char*>(lt) - buf);
            const size_t used = markup(buf + i, n - i);
            if (used == 0) return i;   // incomplete; caller keeps the tail
            i += used;
        }
        return n;
    }

    bool sawRoot() const { return root_; }

private:
    static size_t findSeq(const char* p, size_t n, size_t from, const char* seq) {
        const size_t sl = std::strlen(seq);
        for (size_t i = from; i + sl <= n; ++i) {
            const void* hit = std::memchr(p + i, seq[0], n - i);
            if (!hit) return SIZE_MAX;
            i = size_t(static_cast<const char*>(hit) - p);
            if (i + sl <= n && std::memcmp(p + i, seq, sl) == 0) return i;
        }
        return SIZE_MAX;
    }

    static bool startsWith(const char* p, size_t n, const char* s) {
        const size_t l = std::strlen(s);
        return n >= l && std::memcmp(p, s, l) == 0;
    }

    // p[0] == '<'. Returns length consumed, or 0 if more input is needed.
    size_t markup(const char* p, size_t n) {
        if (n < 2) return 0;
        if (startsWith(p, n, "<!--")) { size_t e = findSeq(p, n, 4, "-->"); return e == SIZE_MAX ? 0 : e + 3; }
        if (startsWith(p, n, "<![CDATA[")) { size_t e = findSeq(p, n, 9, "]]>"); return e == SIZE_MAX ? 0 : e + 3; }
        if (n < 9 && (std::memcmp(p, "<![CDATA[", n) == 0 || std::memcmp(p, "<!--", n < 4 ? n : 4) == 0)) return 0;
        if (p[1] == '?') { size_t e = findSeq(p, n, 2, "?>"); return e == SIZE_MAX ? 0 : e + 2; }
        if (p[1] == '!') {   // DOCTYPE and friends, with an optional [...] internal subset
            int depth = 0;
            for (size_t i = 2; i < n; ++i) {
                if (p[i] == '[') ++depth;
                else if (p[i] == ']') --depth;
                else if (p[i] == '>' && depth <= 0) return i + 1;
            }
            return 0;
        }
        char q = 0;
        size_t i = 1;
        for (; i < n; ++i) {
            const char c = p[i];
            if (q) { if (c == q) q = 0; }
            else if (c == '"' || c == '\'') q = c;
            else if (c == '>') break;
        }
        if (i >= n) return 0;
        tag(p + 1, i - 1);
        return i + 1;
    }

    void tag(const char* p, size_t n) {
        const bool end = n > 0 && p[0] == '/';
        size_t s = end ? 1 : 0;
        size_t e = s;
        while (e < n && !isSpace(p[e]) && p[e] != '/') ++e;
        const char* name = p + s;
        const size_t nl = e - s;
        const bool selfClose = !end && n > 0 && p[n - 1] == '/';
        auto named = [&](const char* x) { return std::strlen(x) == nl && std::memcmp(name, x, nl) == 0; };
        if (end) {
            if (named("COLLECTION") && collection_ > 0) --collection_;
            return;
        }
        if (named("DJ_PLAYLISTS")) root_ = true;
        else if (named("COLLECTION")) { if (!selfClose) ++collection_; }
        else if (named("TRACK") && collection_ > 0) track(p + e, n - e - (selfClose ? 1 : 0));
    }

    void track(const char* p, size_t n) {
        TrackMetadata md;
        bool haveId = false;
        md.source = "xml";
        forEachAttribute(p, n, [&](const char* a, size_t al, std::string v) {
            auto is = [&](const char* x) { return std::strlen(x) == al && std::memcmp(a, x, al) == 0; };
            if (is("TrackID")) haveId = parseU32(v, md.rekordboxId);
            else if (is("Name")) md.title = std::move(v);
            else if (is("Artist")) md.artist = std::move(v);
            else if (is("Album")) md.album = std::move(v);
            else if (is("Genre")) md.genre = std::move(v);
            else if (is("Tonality")) md.key = std::move(v);
            else if (is("Label")) md.label = std::move(v);
            else if (is("AverageBpm")) { double d; if (parseDecimal(v, d) && d >= 0) md.bpm = d; }
            else if (is("TotalTime")) { double d; if (parseDecimal(v, d) && d >= 0) md.durationS = d; }
        });
        if (haveId) out_[md.rekordboxId] = std::move(md);
    }

    std::map<uint32_t, TrackMetadata>& out_;
    int collection_ = 0;
    bool root_ = false;
};

bool finish(const Loader& l, size_t leftover, std::string* error) {
    if (!l.sawRoot()) { if (error) *error = "not a rekordbox XML export (no DJ_PLAYLISTS element)"; return false; }
    if (leftover) { if (error) *error = "truncated or malformed XML near end of file"; return false; }
    return true;
}

} // namespace

bool RekordboxXml::loadFromString(const std::string& xml, std::string* error) {
    tracks_.clear();
    Loader l(tracks_);
    const size_t used = l.feed(xml.data(), xml.size());
    return finish(l, xml.size() - used, error);
}

bool RekordboxXml::load(const std::string& path, std::string* error) {
    tracks_.clear();
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { if (error) *error = "cannot open " + path; return false; }
    Loader l(tracks_);
    std::string buf;
    std::string chunk(kChunk, '\0');
    bool corrupt = false;
    for (;;) {
        const size_t got = std::fread(&chunk[0], 1, chunk.size(), f);
        if (got == 0) break;
        buf.append(chunk.data(), got);
        const size_t used = l.feed(buf.data(), buf.size());
        buf.erase(0, used);
        if (buf.size() > kMaxMarkup) { corrupt = true; break; }
    }
    const bool readError = std::ferror(f) != 0;
    std::fclose(f);
    if (readError) { if (error) *error = "read error on " + path; return false; }
    if (corrupt) { if (error) *error = "malformed XML: unterminated markup"; return false; }
    return finish(l, buf.size(), error);
}

const TrackMetadata* RekordboxXml::find(uint32_t trackId) const {
    auto it = tracks_.find(trackId);
    return it == tracks_.end() ? nullptr : &it->second;
}

} // namespace shunt::meta
