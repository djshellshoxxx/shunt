// ShuntSim (RS-05, FS-TEST section 2): emits a scripted 4-deck plus mixer player network on real UDP.
// Announce every 1.5 s, status every 200 ms, beats at the effective tempo.
// Scenarios live in scenarios/*.cpp and register with REGISTER_SCENARIO.
#include "Sim.h"
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace shuntsim;

namespace {

void usage() {
    std::printf("shuntsim [--dest IP] [--record out.pcapng] [--scenario NAME] [--duration S] [--seed N]\n"
                "         [--jitter-ms X] [--loss PCT] [--outlier-pct PCT] [--time-scale X] [--verbose] [--quiet]\n"
                "         [--key-bytes B0,B1,B2 (key scenario)] [--bar-shift N (wrongbar scenario)]\n"
                "At least one of --dest and --record is required. --time-scale 0 runs as fast as possible.\n"
                "Scenarios:");
    for (auto& s : scenarioRegistry()) std::printf(" %s", s.name);
    std::printf("\n");
}

// Bytes are hex: "8a,0,1", "0x8a,0x00,0x01" or a single 6-digit string "8a0001".
bool parseKeyBytes(const std::string& s, uint8_t out[3]) {
    std::string t;
    for (char c : s) t += (c == ':' || c == ';') ? ',' : c;
    if (t.find(',') == std::string::npos) {
        if (t.size() != 6) return false;
        for (int i = 0; i < 3; ++i) {
            char* end = nullptr;
            const std::string h = t.substr(size_t(i) * 2, 2);
            const long v = std::strtol(h.c_str(), &end, 16);
            if (*end) return false;
            out[i] = uint8_t(v);
        }
        return true;
    }
    size_t pos = 0;
    for (int i = 0; i < 3; ++i) {
        const size_t comma = t.find(',', pos);
        if ((i < 2) != (comma != std::string::npos)) return false;
        const std::string tok = t.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        char* end = nullptr;
        const long v = std::strtol(tok.c_str(), &end, 16);
        if (tok.empty() || *end || v < 0 || v > 255) return false;
        out[i] = uint8_t(v);
        pos = comma + 1;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--dest") o.dest = next();
        else if (a == "--scenario") o.scenario = next();
        else if (a == "--duration") o.durationS = std::atof(next().c_str());
        else if (a == "--jitter-ms") o.jitterMs = std::atof(next().c_str());
        else if (a == "--loss") o.lossPct = std::atof(next().c_str());
        else if (a == "--outlier-pct") o.outlierPct = std::atof(next().c_str());
        else if (a == "--seed") o.seed = unsigned(std::atoi(next().c_str()));
        else if (a == "--record") o.record = next();
        else if (a == "--time-scale") o.timeScale = std::atof(next().c_str());
        else if (a == "--bar-shift") { o.barShift = std::atoi(next().c_str()); o.barShiftSet = true; }
        else if (a == "--key-bytes") {
            if (!parseKeyBytes(next(), o.keyBytes)) { std::fprintf(stderr, "bad --key-bytes\n"); return 2; }
            o.keyBytesSet = true;
        }
        else if (a == "--verbose") o.verbose = true;
        else if (a == "--quiet") o.quiet = true;
        else { usage(); return a == "--help" ? 0 : 2; }
    }
    Sim sim(o);
    std::string err;
    if (!sim.init(&err)) { std::fprintf(stderr, "shuntsim: %s\n", err.c_str()); return 2; }
    sim.run();
    sim.summary();
    return 0;
}
