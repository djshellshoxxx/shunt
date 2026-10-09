#include "shunt/app/Settings.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace shunt::app {

namespace {
template <class T> T clampv(T v, T lo, T hi) { return std::min(hi, std::max(lo, v)); }
}

Settings Settings::fromJson(const Json& j) { return fromJson(j, Settings{}); }

Settings Settings::fromJson(const Json& j, const Settings& base) {
    Settings s = base;
    if (!j.isObject()) return s;
    for (auto& kv : j.members()) s.raw[kv.first] = kv.second;
    if (j.has("interface")) s.interfaceName = j.get("interface").asString("");
    if (j.has("mode")) s.mode = j.get("mode").asString("follow") == "passive" ? "passive" : "follow";
    if (j.has("deviceNumber")) s.deviceNumber = clampv(j.get("deviceNumber").asInt(5), 1, 6);
    if (j.has("latencyMs")) s.latencyMs = clampv(j.get("latencyMs").asNumber(1.0), -20.0, 50.0);
    if (j.has("barOffset")) s.barOffset = j.get("barOffset").asInt(0) & 3;
    if (j.has("beatOnly")) s.beatOnly = j.get("beatOnly").asBool(false);
    if (j.has("profile")) s.profile = j.get("profile").asString("stage") == "rehearsal" ? "rehearsal" : "stage";
    if (j.has("venue")) s.venue = j.get("venue").asString("");
    if (j.has("performer")) s.performer = j.get("performer").asString("");
    const Json& l = j.get("link");
    if (l.has("enabled")) s.outputs.link.enabled = l.get("enabled").asBool(false);
    const Json& m = j.get("midi");
    if (m.has("enabled")) s.outputs.midi.enabled = m.get("enabled").asBool(false);
    if (m.has("port")) s.outputs.midi.port = m.get("port").asString("");
    if (m.has("pause")) s.outputs.midi.options.pause = m.get("pause").asString("keep") == "stop" ? out::MidiClockOptions::Stop : out::MidiClockOptions::KeepRunning;
    if (m.has("startOnReset")) s.outputs.midi.options.startOnReset = m.get("startOnReset").asBool(false);
    if (m.has("sppOnReset")) s.outputs.midi.options.sppOnReset = m.get("sppOnReset").asBool(false);
    if (m.has("startOnFirstLock")) s.outputs.midi.options.startOnFirstLock = m.get("startOnFirstLock").asBool(true);
    const Json& o = j.get("osc");
    if (o.has("enabled")) s.outputs.osc.enabled = o.get("enabled").asBool(false);
    if (o.has("host")) s.outputs.osc.host = o.get("host").asString("127.0.0.1");
    if (o.has("port")) s.outputs.osc.port = uint16_t(clampv(o.get("port").asInt(9000), 1, 65535));
    if (o.has("profile")) s.outputs.osc.profile = out::oscProfileFromName(o.get("profile").asString("generic"));
    if (o.has("qlabMasterCue")) s.outputs.osc.qlabMasterCue = clampv(o.get("qlabMasterCue").asInt(1), 0, 9999);
    if (o.has("qlabTrackCue")) s.outputs.osc.qlabTrackCue = clampv(o.get("qlabTrackCue").asInt(2), 0, 9999);
    if (o.has("inEnabled")) s.oscInEnabled = o.get("inEnabled").asBool(false);
    if (o.has("inPort")) s.oscInPort = clampv(o.get("inPort").asInt(9100), 1, 65535);
    const Json& h = j.get("http");
    if (h.has("port")) s.httpPort = clampv(h.get("port").asInt(8080), 1, 65535);
    if (h.has("bind")) s.httpBind = h.get("bind").asString("0.0.0.0");
    return s;
}

Json Settings::toJson() const {
    Json j = raw.isObject() ? raw : Json::object();
    j["interface"] = interfaceName;
    j["mode"] = mode;
    j["deviceNumber"] = deviceNumber;
    j["latencyMs"] = latencyMs;
    j["barOffset"] = barOffset;
    j["beatOnly"] = beatOnly;
    j["profile"] = profile;
    j["venue"] = venue;
    j["performer"] = performer;
    j["link"]["enabled"] = outputs.link.enabled;
    Json& m = j["midi"];
    m["enabled"] = outputs.midi.enabled;
    m["port"] = outputs.midi.port;
    m["pause"] = outputs.midi.options.pause == out::MidiClockOptions::Stop ? "stop" : "keep";
    m["startOnReset"] = outputs.midi.options.startOnReset;
    m["sppOnReset"] = outputs.midi.options.sppOnReset;
    m["startOnFirstLock"] = outputs.midi.options.startOnFirstLock;
    Json& o = j["osc"];
    o["enabled"] = outputs.osc.enabled;
    o["host"] = outputs.osc.host;
    o["port"] = int(outputs.osc.port);
    o["profile"] = out::oscProfileName(outputs.osc.profile);
    o["qlabMasterCue"] = outputs.osc.qlabMasterCue;
    o["qlabTrackCue"] = outputs.osc.qlabTrackCue;
    o["inEnabled"] = oscInEnabled;
    o["inPort"] = oscInPort;
    j["http"]["port"] = httpPort;
    j["http"]["bind"] = httpBind;
    return j;
}

std::string Settings::defaultPath() {
    if (const char* x = std::getenv("XDG_CONFIG_HOME")) if (*x) return std::string(x) + "/shunt/config.json";
    if (const char* h = std::getenv("HOME")) if (*h) return std::string(h) + "/.config/shunt/config.json";
    return "shunt-config.json";
}

Settings Settings::load(const std::string& path, std::string* error) {
    std::ifstream f(path);
    if (!f) return Settings{};
    std::stringstream ss;
    ss << f.rdbuf();
    Json j;
    std::string err;
    if (!Json::parse(ss.str(), j, &err)) { if (error) *error = "config.json: " + err; return Settings{}; }
    return fromJson(j);
}

bool Settings::save(const std::string& path, std::string* error) const {
    for (size_t i = 1; i < path.size(); ++i)
        if (path[i] == '/') ::mkdir(path.substr(0, i).c_str(), 0755);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) { if (error) *error = "cannot write " + tmp; return false; }
        f << toJson().dump() << "\n";
        if (!f) { if (error) *error = "write failed"; return false; }
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) { if (error) *error = "rename failed"; return false; }
    return true;
}

} // namespace shunt::app
