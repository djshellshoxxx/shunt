// User settings (RS-03 "Settings storage"), serialised as JSON. Desktop stores
// them in the user settings folder, the Box in /data/config.json.
#pragma once
#include "shunt/clock/ClockEngine.h"
#include "shunt/net/ClaimMachine.h"
#include "shunt/out/Outputs.h"
#include <cstdint>
#include <map>
#include <string>

namespace shunt::core {

struct Settings {
    // network
    std::string interfaceName;                 // empty = auto (interface that sees keep-alives)
    net::Mode mode = net::Mode::Follow;
    uint8_t leadNumber = 5;                    // Lead (metadata) mode: 1..6
    // clock
    clock::Profile profile = clock::Profile::Stage;
    int64_t latencyOffsetNs = 1'000'000;       // global, -20 ms .. +50 ms
    int userBarOffset = 0;                     // 0..3
    bool beatOnly = false;
    bool rememberBarOffsets = true;            // bar offset memory per (rekordbox id, slot)
    std::map<std::string, int> barOffsetMemory; // key "slot:rekordboxId" -> 0..3
    // outputs
    out::OutputsConfig outputs;
    // OSC input (/shunt/baroffset i, /shunt/beatonly i, /shunt/follow i)
    bool oscInputEnabled = true;
    uint16_t oscInputPort = 7001;
    // web server (status, overlay, Box config)
    bool webEnabled = true;
    uint16_t webPort = 8080;                   // Box uses 80
    // tracklist
    std::string dataDir;                       // tracklists, captures; empty = platform default
    std::string exportFormat = "csv";          // csv, cue, txt, ndjson, report
    std::string performer, venue;
    std::string rekordboxXmlPath;
    // desktop UI
    bool startMinimised = false;
    bool autoStart = false;
    bool firstRunDone = false;
    bool stageMode = false;
    std::map<std::string, std::string> midiLearn;   // control id -> "cc:ch:num" / "note:ch:num"
    std::string windowState;

    std::string toJson() const;
    static Settings fromJson(const std::string& json, std::string* error = nullptr);   // missing keys keep defaults
    bool save(const std::string& path) const;          // atomic write (tmp + rename)
    static Settings load(const std::string& path);     // defaults if missing or broken
};

std::string defaultDataDir();                          // ~/Documents/Shunt, %USERPROFILE%\Documents\Shunt, /data on the Box
std::string defaultSettingsPath();                     // per-user config folder + "/settings.json"

} // namespace shunt::core
