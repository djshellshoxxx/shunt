#include "shunt/app/Json.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace shunt::app {

namespace {
const Json kNull;
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o += char(c);
        }
    }
    return o;
}

Json& Json::operator[](const std::string& key) {
    if (type_ != Object) { *this = object(); }
    for (auto& kv : obj_) if (kv.first == key) return kv.second;
    obj_.emplace_back(key, Json());
    return obj_.back().second;
}

const Json& Json::get(const std::string& key) const {
    if (type_ == Object) for (auto& kv : obj_) if (kv.first == key) return kv.second;
    return kNull;
}

bool Json::has(const std::string& key) const {
    if (type_ == Object) for (auto& kv : obj_) if (kv.first == key) return true;
    return false;
}

void Json::erase(const std::string& key) {
    for (auto it = obj_.begin(); it != obj_.end(); ++it) if (it->first == key) { obj_.erase(it); return; }
}

void Json::write(std::string& o) const {
    switch (type_) {
    case Null: o += "null"; break;
    case Bool: o += b_ ? "true" : "false"; break;
    case Number: {
        if (!std::isfinite(n_)) { o += "null"; break; }
        char b[40];
        if (n_ == std::floor(n_) && std::fabs(n_) < 1e15) std::snprintf(b, sizeof b, "%lld", (long long)n_);
        else std::snprintf(b, sizeof b, "%.6g", n_);
        o += b;
        break;
    }
    case String: o += '"'; o += jsonEscape(s_); o += '"'; break;
    case Array:
        o += '[';
        for (size_t i = 0; i < arr_.size(); ++i) { if (i) o += ','; arr_[i].write(o); }
        o += ']';
        break;
    case Object:
        o += '{';
        for (size_t i = 0; i < obj_.size(); ++i) {
            if (i) o += ',';
            o += '"'; o += jsonEscape(obj_[i].first); o += "\":";
            obj_[i].second.write(o);
        }
        o += '}';
        break;
    }
}

std::string Json::dump() const { std::string o; write(o); return o; }

namespace {
struct Parser {
    const std::string& s;
    explicit Parser(const std::string& t) : s(t) {}
    size_t p = 0;
    std::string err;
    int depth = 0;
    void ws() { while (p < s.size() && (s[p] == ' ' || s[p] == '\n' || s[p] == '\r' || s[p] == '\t')) ++p; }
    bool fail(const char* m) { if (err.empty()) err = std::string(m) + " at " + std::to_string(p); return false; }
    bool lit(const char* w) { size_t n = std::char_traits<char>::length(w); if (s.compare(p, n, w) == 0) { p += n; return true; } return false; }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += char(cp);
        else if (cp < 0x800) { o += char(0xC0 | (cp >> 6)); o += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += char(0xE0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
        else { o += char(0xF0 | (cp >> 18)); o += char(0x80 | ((cp >> 12) & 0x3F)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
    }
    bool hex4(unsigned& v) {
        if (p + 4 > s.size()) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s[p++]; v <<= 4;
            if (c >= '0' && c <= '9') v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool str(std::string& o) {
        ++p;   // opening quote
        while (p < s.size()) {
            char c = s[p++];
            if (c == '"') return true;
            if (c != '\\') { o += c; continue; }
            if (p >= s.size()) break;
            char e = s[p++];
            switch (e) {
            case 'n': o += '\n'; break;
            case 'r': o += '\r'; break;
            case 't': o += '\t'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'u': {
                unsigned cp;
                if (!hex4(cp)) return fail("bad \\u escape");
                if (cp >= 0xD800 && cp < 0xDC00 && s.compare(p, 2, "\\u") == 0) {
                    p += 2; unsigned lo;
                    if (!hex4(lo)) return fail("bad \\u escape");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(o, cp);
                break;
            }
            default: o += e;
            }
        }
        return fail("unterminated string");
    }
    bool value(Json& out) {
        ws();
        if (p >= s.size()) return fail("unexpected end");
        if (++depth > 64) return fail("too deep");
        struct D { int& d; ~D() { --d; } } guard{depth};
        char c = s[p];
        if (c == '{') {
            ++p; out = Json::object(); ws();
            if (p < s.size() && s[p] == '}') { ++p; return true; }
            for (;;) {
                ws();
                if (p >= s.size() || s[p] != '"') return fail("expected key");
                std::string k;
                if (!str(k)) return false;
                ws();
                if (p >= s.size() || s[p] != ':') return fail("expected ':'");
                ++p;
                Json v;
                if (!value(v)) return false;
                out[k] = std::move(v);
                ws();
                if (p < s.size() && s[p] == ',') { ++p; continue; }
                if (p < s.size() && s[p] == '}') { ++p; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++p; out = Json::array(); ws();
            if (p < s.size() && s[p] == ']') { ++p; return true; }
            for (;;) {
                Json v;
                if (!value(v)) return false;
                out.push(std::move(v));
                ws();
                if (p < s.size() && s[p] == ',') { ++p; continue; }
                if (p < s.size() && s[p] == ']') { ++p; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') { std::string v; if (!str(v)) return false; out = Json(std::move(v)); return true; }
        if (lit("true")) { out = Json(true); return true; }
        if (lit("false")) { out = Json(false); return true; }
        if (lit("null")) { out = Json(); return true; }
        const char* b = s.c_str() + p;
        char* e = nullptr;
        double v = std::strtod(b, &e);
        if (e == b) return fail("unexpected character");
        p += size_t(e - b);
        out = Json(v);
        return true;
    }
};
}

bool Json::parse(const std::string& text, Json& out, std::string* error) {
    Parser ps(text);
    Json v;
    bool ok = ps.value(v);
    if (ok) { ps.ws(); if (ps.p != text.size()) ok = ps.fail("trailing data"); }
    if (ok) out = std::move(v);
    else if (error) *error = ps.err;
    return ok;
}

} // namespace shunt::app
