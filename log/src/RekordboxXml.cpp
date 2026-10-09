#include "shunt/log/RekordboxXml.h"
#include <cstdlib>

namespace shunt::log {

namespace {
std::string decode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { o += s[i]; continue; }
        const size_t e = s.find(';', i);
        if (e == std::string::npos || e - i > 10) { o += s[i]; continue; }
        const std::string ent = s.substr(i + 1, e - i - 1);
        if (ent == "amp") o += '&';
        else if (ent == "lt") o += '<';
        else if (ent == "gt") o += '>';
        else if (ent == "quot") o += '"';
        else if (ent == "apos") o += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            const unsigned long cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? std::strtoul(ent.c_str() + 2, nullptr, 16) : std::strtoul(ent.c_str() + 1, nullptr, 10);
            if (cp < 0x80) o += char(cp);
            else if (cp < 0x800) { o += char(0xC0 | (cp >> 6)); o += char(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) { o += char(0xE0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
            else { o += char(0xF0 | (cp >> 18)); o += char(0x80 | ((cp >> 12) & 0x3F)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
        } else { o += s[i]; continue; }
        i = e;
    }
    return o;
}
}

MetaMap parseRekordboxXml(const std::string& xml) {
    MetaMap out;
    size_t p = 0;
    while ((p = xml.find("<TRACK", p)) != std::string::npos) {
        p += 6;
        if (p >= xml.size() || (xml[p] != ' ' && xml[p] != '\t' && xml[p] != '\n' && xml[p] != '\r')) continue;   // <TRACKS ...> etc.
        // Attributes up to the closing '>' (quotes may contain '>').
        std::map<std::string, std::string> attr;
        while (p < xml.size() && xml[p] != '>') {
            while (p < xml.size() && (xml[p] == ' ' || xml[p] == '\t' || xml[p] == '\n' || xml[p] == '\r' || xml[p] == '/')) ++p;
            const size_t ns = p;
            while (p < xml.size() && xml[p] != '=' && xml[p] != '>' && xml[p] != ' ' && xml[p] != '/') ++p;
            if (p >= xml.size() || xml[p] != '=') continue;
            const std::string name = xml.substr(ns, p - ns);
            ++p;
            if (p >= xml.size() || (xml[p] != '"' && xml[p] != '\'')) continue;
            const char q = xml[p++];
            const size_t ve = xml.find(q, p);
            if (ve == std::string::npos) return out;
            attr[name] = decode(xml.substr(p, ve - p));
            p = ve + 1;
        }
        auto id = attr.find("TrackID");
        auto nm = attr.find("Name");
        if (id == attr.end() || nm == attr.end()) continue;
        const unsigned long v = std::strtoul(id->second.c_str(), nullptr, 10);
        if (v == 0 || v > 0xffffffffUL) continue;
        TrackMeta m;
        m.title = nm->second;
        m.artist = attr["Artist"]; m.album = attr["Album"]; m.genre = attr["Genre"]; m.key = attr["Tonality"];
        m.bpm = std::atof(attr["AverageBpm"].c_str());
        m.durationS = std::atof(attr["TotalTime"].c_str());
        out[uint32_t(v)] = std::move(m);
    }
    return out;
}

void applyMeta(std::vector<Row>& rows, const MetaMap& meta) {
    for (auto& r : rows) {
        auto it = meta.find(r.rekordboxId);
        if (it == meta.end()) continue;
        r.title = it->second.title; r.artist = it->second.artist; r.album = it->second.album;
        r.genre = it->second.genre; r.key = it->second.key;
        if (it->second.bpm > 0) r.bpm = it->second.bpm;
        if (it->second.durationS > 0) r.durationS = it->second.durationS;
        r.source = "rekordbox-xml";
    }
}

} // namespace shunt::log
