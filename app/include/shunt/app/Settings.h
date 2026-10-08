// Settings (RS-07 section 6): JSON file, unknown keys preserved, values clamped.
#pragma once
#include "shunt/app/Json.h"
#include "shunt/out/OutputManager.h"
#include <string>

namespace shunt::app {

struct Settings {
    std::string interfaceName;           // empty: not chosen yet (first run)
    std::string mode = "follow";         // follow | passive
    int deviceNumber = 5;
    double latencyMs = 1.0;
    int barOffset = 0;
    bool beatOnly = false;
    std::string profile = "stage";       // stage | rehearsal
    std::string venue, performer;
    out::OutputSettings outputs;
    int httpPort = 8080;
    std::string httpBind = "0.0.0.0";
    Json raw = Json::object();           // everything read from disk, so unknown keys survive

    static Settings fromJson(const Json& j);
    static Settings fromJson(const Json& j, const Settings& base);
    Json toJson() const;
    static std::string defaultPath();
    static Settings load(const std::string& path, std::string* error = nullptr);
    bool save(const std::string& path, std::string* error = nullptr) const;
};

} // namespace shunt::app
